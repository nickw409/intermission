package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/invites"
)

// `recs-api admin sessions revoke-all` (deploy/runbooks/revoke-sessions.md):
// after a suspected session-token leak the owner logs everyone out by
// deleting every row in the sessions table. Nothing else changes.

// seedUser inserts a user with one rating and n sessions, and returns the
// session tokens (as the browser would hold them in the cookie).
func seedUser(t *testing.T, d *app.Deps, name string, n int) []string {
	t.Helper()
	ctx := context.Background()
	now := time.Now().Unix()
	res, err := d.DB.Writer.ExecContext(ctx,
		"INSERT INTO users (username, pw_hash, created_at) VALUES (?, ?, ?)", name, "$argon2id$x", now)
	if err != nil {
		t.Fatal(err)
	}
	uid, err := res.LastInsertId()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := d.DB.Writer.ExecContext(ctx,
		"INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)", uid, 1, 4.5, now); err != nil {
		t.Fatal(err)
	}
	var toks []string
	for range n {
		var raw [32]byte
		if _, err := rand.Read(raw[:]); err != nil {
			t.Fatal(err)
		}
		tok := base64.RawURLEncoding.EncodeToString(raw[:])
		hash := sha256.Sum256([]byte(tok))
		if _, err := d.DB.Writer.ExecContext(ctx,
			"INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
			hash[:], uid, now, now+3600); err != nil {
			t.Fatal(err)
		}
		toks = append(toks, tok)
	}
	return toks
}

func countRows(t *testing.T, d *app.Deps, table string) int {
	t.Helper()
	var n int
	// table is a constant from the test, never input.
	if err := d.DB.Reader.QueryRow("SELECT count(*) FROM " + table).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func TestAdminSessionsRevokeAll(t *testing.T) {
	d := migratedAdminDeps(t)
	ctx := context.Background()
	toks := append(seedUser(t, d, "alice", 2), seedUser(t, d, "bob", 1)...)
	if _, err := invites.New(d.DB).Create(ctx, 1, 0, "carol"); err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	if err := runAdminTo(ctx, d, []string{"sessions", "revoke-all"}, &out); err != nil {
		t.Fatal(err)
	}
	if got := out.String(); got != "revoked 3 sessions\n" {
		t.Errorf("stdout = %q, want %q", got, "revoked 3 sessions\n")
	}
	for _, tok := range toks {
		if strings.Contains(out.String(), tok) {
			t.Error("a session token was printed")
		}
	}
	if n := countRows(t, d, "sessions"); n != 0 {
		t.Errorf("%d sessions left", n)
	}
	for table, want := range map[string]int{"users": 2, "ratings": 2, "invites": 1} {
		if n := countRows(t, d, table); n != want {
			t.Errorf("%s: %d rows, want %d", table, n, want)
		}
	}
}

func TestAdminSessionsRevokeAllEmpty(t *testing.T) {
	d := migratedAdminDeps(t)
	var out bytes.Buffer
	if err := runAdminTo(context.Background(), d, []string{"sessions", "revoke-all"}, &out); err != nil {
		t.Fatal(err)
	}
	if got := out.String(); got != "revoked 0 sessions\n" {
		t.Errorf("stdout = %q", got)
	}
}

// The tokens never reach the log either (CLAUDE.md, security).
func TestAdminSessionsRevokeAllLogsNoTokens(t *testing.T) {
	d := migratedAdminDeps(t)
	toks := seedUser(t, d, "alice", 2)
	var logs bytes.Buffer
	d.Logger = slog.New(slog.NewTextHandler(&logs, nil))
	var out bytes.Buffer
	if err := runAdminTo(context.Background(), d, []string{"sessions", "revoke-all"}, &out); err != nil {
		t.Fatal(err)
	}
	for _, tok := range toks {
		if strings.Contains(logs.String(), tok) {
			t.Errorf("token logged: %s", logs.String())
		}
	}
	if !strings.Contains(logs.String(), "sessions revoked") || !strings.Contains(logs.String(), "count=2") {
		t.Errorf("log lacks the revocation record: %s", logs.String())
	}
}

func TestAdminSessionsUsage(t *testing.T) {
	d := migratedAdminDeps(t)
	seedUser(t, d, "alice", 1)
	for _, args := range [][]string{
		nil,
		{"sessions"},
		{"sessions", "foo"},
		{"sessions", "revoke"},
		{"sessions", "revoke-all", "extra"},
		{"sessions", "revoke-all", "--force"},
	} {
		var out bytes.Buffer
		err := runAdminTo(context.Background(), d, args, &out)
		if err == nil {
			t.Errorf("%v accepted", args)
			continue
		}
		if !strings.Contains(err.Error(), "admin invite create") || !strings.Contains(err.Error(), "admin sessions revoke-all") {
			t.Errorf("%v: usage does not list both commands: %v", args, err)
		}
		if out.Len() != 0 {
			t.Errorf("%v printed %q", args, out.String())
		}
	}
	if n := countRows(t, d, "sessions"); n != 1 {
		t.Errorf("a rejected command deleted sessions: %d left", n)
	}
}

// Like invite create, revoke-all never migrates: it runs next to a live
// server.
func TestAdminSessionsRefusesToMigrate(t *testing.T) {
	d := adminDeps(t) // un-migrated
	var out bytes.Buffer
	err := runAdminTo(context.Background(), d, []string{"sessions", "revoke-all"}, &out)
	if err == nil {
		t.Fatal("revoke-all ran on an un-migrated database")
	}
	if !strings.Contains(err.Error(), "recs-api migrate") {
		t.Errorf("error does not say what to do: %v", err)
	}
	if out.Len() != 0 {
		t.Errorf("printed %q", out.String())
	}
	var objects int
	if err := d.DB.Reader.QueryRow("SELECT count(*) FROM sqlite_master").Scan(&objects); err != nil {
		t.Fatal(err)
	}
	if objects != 0 {
		t.Fatalf("revoke-all created %d schema objects", objects)
	}
}

// A cookie that worked before revoke-all gets 401 from the server's session
// middleware afterwards (sessions are looked up in the database on every
// request; nothing is cached).
func TestAdminSessionsRevokeAllRejectsCookie(t *testing.T) {
	d := migratedAdminDeps(t)
	tok := seedUser(t, d, "alice", 1)[0]
	h := auth.RequireSession(d)(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	get := func() int {
		req := httptest.NewRequest("GET", "/api/me", nil)
		req.AddCookie(&http.Cookie{Name: auth.CookieName, Value: tok})
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec.Code
	}
	if code := get(); code != http.StatusNoContent {
		t.Fatalf("before revoke-all: %d", code)
	}
	var out bytes.Buffer
	if err := runAdminTo(context.Background(), d, []string{"sessions", "revoke-all"}, &out); err != nil {
		t.Fatal(err)
	}
	if code := get(); code != http.StatusUnauthorized {
		t.Fatalf("after revoke-all: %d, want 401", code)
	}
}
