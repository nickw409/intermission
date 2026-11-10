package auth

import (
	"context"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
)

// Classic SQL-injection payloads. Each must be treated as literal text in
// every field that reaches SQL.
var sqliCorpus = []string{
	`' OR '1'='1`,
	`' OR '1'='1' --`,
	`' OR 1=1--`,
	`" OR "1"="1`,
	`' OR ''='`,
	`admin'--`,
	`admin' #`,
	`admin'/*`,
	`') OR ('1'='1`,
	`')) OR (('1'='1`,
	`'; DROP TABLE users; --`,
	`'; DELETE FROM sessions; --`,
	`'; UPDATE invites SET used_count = 0; --`,
	`'; INSERT INTO users VALUES (99,'evil','x',0,0); --`,
	`' UNION SELECT id, pw_hash FROM users --`,
	`' UNION SELECT NULL, NULL --`,
	`' UNION ALL SELECT name, sql FROM sqlite_master --`,
	`1; ATTACH DATABASE '/tmp/x.db' AS x; --`,
	`' AND 1=(SELECT count(*) FROM users) --`,
	`' AND substr((SELECT pw_hash FROM users LIMIT 1),1,1)='$`,
	`' OR randomblob(100000000) --`,
	`' || (SELECT pw_hash FROM users) || '`,
	`\' OR 1=1 --`,
	`%' OR 1=1 --`,
	`' OR username LIKE '%`,
	`1' ORDER BY 1--`,
	`' GROUP BY username HAVING 1=1 --`,
	`'; PRAGMA writable_schema = 1; --`,
	`'; VACUUM INTO '/tmp/steal.db'; --`,
	"'\x00 OR 1=1",
	`''''''''''`,
	`?; SELECT ?`,
	`$1 OR 1=1`,
	`:name OR 1=1`,
	`x'00'`,
	`char(39) || 'OR 1=1`,
}

// pad makes a payload a valid password (10-128 characters).
func pad(p string) string {
	if len([]rune(p)) < minPassword {
		p += strings.Repeat("_", minPassword-len([]rune(p)))
	}
	return p
}

func TestSQLInjectionCorpusIsLiteralText(t *testing.T) {
	if len(sqliCorpus) < 30 {
		t.Fatalf("corpus has %d payloads, want >= 30", len(sqliCorpus))
	}
	h := newHarness(t, app.SignupInvite)
	code := h.mintInvite(len(sqliCorpus)+1, 0)
	if rec := h.do("POST", "/api/auth/signup", signupBody{"alice", goodPass, code}, ""); rec.Code != 201 {
		t.Fatalf("alice: %d", rec.Code)
	}
	snapshot := func() [4]int {
		return [4]int{
			h.count("SELECT count(*) FROM users"),
			h.count("SELECT count(*) FROM sessions"),
			h.count("SELECT count(*) FROM invites"),
			h.count("SELECT coalesce(sum(used_count), 0) FROM invites"),
		}
	}

	for i, p := range sqliCorpus {
		before := snapshot()

		// As a login username, with alice's real password and as a password
		// for alice: always plain invalid credentials.
		for _, body := range []loginBody{{p, goodPass}, {"alice", p}, {"alice" + p, goodPass}} {
			rec := h.do("POST", "/api/auth/login", body, "")
			if rec.Code != 401 || errOf(t, rec).Code != apperr.CodeInvalidCredentials {
				t.Errorf("login %+v: %d %s", body, rec.Code, rec.Body.String())
			}
		}
		// As an invite code: just an unknown code.
		rec := h.do("POST", "/api/auth/signup", signupBody{"victim", goodPass, p}, "")
		if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeInvalidInvite {
			t.Errorf("invite %q: %d %s", p, rec.Code, rec.Body.String())
		}
		// As a username at signup: refused by the field rules.
		rec = h.do("POST", "/api/auth/signup", signupBody{p, goodPass, code}, "")
		if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeBadRequest {
			t.Errorf("username %q: %d %s", p, rec.Code, rec.Body.String())
		}
		if after := snapshot(); after != before {
			t.Fatalf("payload %q changed the database: %v -> %v", p, before, after)
		}

		// As a password: stored (hashed) and later matched as exact text.
		name := "user" + strings.Repeat("x", i%5) + string(rune('a'+i%26)) + string(rune('a'+i/26))
		rec = h.do("POST", "/api/auth/signup", signupBody{name, pad(p), code}, "")
		if rec.Code != 201 {
			t.Fatalf("signup with password %q: %d %s", p, rec.Code, rec.Body.String())
		}
		if rec := h.do("POST", "/api/auth/login", loginBody{name, pad(p)}, ""); rec.Code != 200 {
			t.Errorf("login with password %q: %d", p, rec.Code)
		}
		if rec := h.do("POST", "/api/auth/login", loginBody{name, pad(p) + " "}, ""); rec.Code != 401 {
			t.Errorf("near-miss password for %q accepted", p)
		}
	}

	// Tables intact; alice's login still works.
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""); rec.Code != 200 {
		t.Fatalf("alice after corpus: %d", rec.Code)
	}
	if n := h.count("SELECT count(*) FROM users"); n != len(sqliCorpus)+1 {
		t.Fatalf("users = %d", n)
	}
}

// Below the HTTP field rules, the storage layer itself keeps arbitrary text
// literal: a payload written as a username comes back byte for byte.
func TestStorageKeepsPayloadsLiteral(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	ctx := context.Background()
	n := 0
	for _, p := range sqliCorpus {
		if strings.ContainsRune(p, 0) {
			// SQLite's length() stops at NUL, so the CHECK sees 1 character;
			// the HTTP rules reject NUL in usernames long before this.
			continue
		}
		n++
		name := p
		for len(name) < 3 {
			name += "_"
		}
		if len(name) > 32 {
			name = name[:32]
		}
		if _, err := h.db.Writer.ExecContext(ctx, "INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', 0)", name); err != nil {
			t.Fatalf("insert %q: %v", name, err)
		}
		var got string
		if err := h.db.Reader.QueryRowContext(ctx, "SELECT username FROM users WHERE username = ?", name).Scan(&got); err != nil || got != name {
			t.Fatalf("read back %q: %q, %v", name, got, err)
		}
		taken, err := h.svc.usernameTaken(ctx, h.db.Reader, name)
		if err != nil || !taken {
			t.Fatalf("usernameTaken(%q) = %v, %v", name, taken, err)
		}
	}
	if got := h.count("SELECT count(*) FROM users"); got != n {
		t.Fatalf("users = %d, want %d", got, n)
	}
}
