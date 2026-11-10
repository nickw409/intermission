package auth

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/contractsdir"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// Replays contracts/fixtures/public/{signup,login,logout,getMe,deleteMe}.json
// against the real handlers. Each case runs on a fresh database seeded so the
// case's precondition holds. Placeholder values in the fixtures (the session
// token) are swapped for real ones; status, error bodies, success bodies and
// cookie attributes must match exactly.

type fixtureFile struct {
	OperationID string        `json:"operationId"`
	Cases       []fixtureCase `json:"cases"`
}

type fixtureCase struct {
	Name    string `json:"name"`
	Request struct {
		Method       string            `json:"method"`
		Path         string            `json:"path"`
		Headers      map[string]string `json:"headers"`
		Body         json.RawMessage   `json:"body"`
		RawBody      *string           `json:"rawBody"`
		RawBodyBytes int               `json:"rawBodyBytes"`
	} `json:"request"`
	Response struct {
		Status  int               `json:"status"`
		Headers map[string]string `json:"headers"`
		Body    json.RawMessage   `json:"body"`
	} `json:"response"`
}

const fixtureToken = "fixture-session-token"

// seedAlice creates alice (fixture password, created at the fixture time,
// 5 ratings) and returns a live session token for her.
func seedAlice(t *testing.T, h *harness) string {
	t.Helper()
	ctx := context.Background()
	phc, err := h.svc.hasher.Hash(ctx, goodPass)
	if err != nil {
		t.Fatal(err)
	}
	res, err := h.db.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('alice', ?, 1794256680)", phc)
	if err != nil {
		t.Fatal(err)
	}
	uid, _ := res.LastInsertId()
	for m := 1; m <= 5; m++ {
		if _, err := h.db.Writer.Exec("INSERT INTO ratings VALUES (?, ?, 4.0, 0)", uid, m); err != nil {
			t.Fatal(err)
		}
	}
	tx, err := h.db.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	tok, err := h.svc.sessions.create(ctx, tx, uid, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	return tok
}

func seedInvite(t *testing.T, h *harness, code string, uses int, expiresAt *int64) {
	t.Helper()
	_, err := h.db.Writer.Exec("INSERT INTO invites (code_hash, max_uses, used_count, expires_at, note, created_at) VALUES (?, ?, 0, ?, 'fixture', 0)",
		invites.Hash(code), uses, expiresAt)
	if err != nil {
		t.Fatal(err)
	}
}

func TestContractFixtures(t *testing.T) {
	dir, err := contractsdir.Fixtures("public")
	if err != nil {
		t.Fatal(err)
	}
	for _, op := range []string{"signup", "login", "logout", "getMe", "deleteMe"} {
		raw, err := os.ReadFile(filepath.Join(dir, op+".json"))
		if err != nil {
			t.Fatal(err)
		}
		var ff fixtureFile
		if err := json.Unmarshal(raw, &ff); err != nil {
			t.Fatal(err)
		}
		if ff.OperationID != op || len(ff.Cases) == 0 {
			t.Fatalf("%s: bad fixture file", op)
		}
		for _, c := range ff.Cases {
			t.Run(op+"/"+c.Name, func(t *testing.T) { runFixtureCase(t, op, c) })
		}
	}
}

func runFixtureCase(t *testing.T, op string, c fixtureCase) {
	h := newHarness(t, app.SignupInvite, func(o *options) {
		if c.Name == "rate_limited" { // the production limits, exhausted below
			o.SignupGlobal = ratelimit.SignupGlobal
			o.LoginPerAccount = ratelimit.LoginPerAccount
		}
	})
	past := int64(1794256680 - 3600)
	seedInvite(t, h, "k7Qm2xVb9pLs", 10, nil)
	seedInvite(t, h, "expired-code", 10, &past)

	token := ""
	switch {
	case op == "signup" && c.Name == "username_taken":
		seedAlice(t, h)
	case op != "signup":
		token = seedAlice(t, h)
	}
	if c.Name == "rate_limited" {
		switch op {
		case "signup":
			for ok, _ := h.svc.signupGlobal.Allow(); ok; ok, _ = h.svc.signupGlobal.Allow() {
			}
		case "login":
			for ok, _ := h.svc.loginPerAccount.Allow("alice"); ok; ok, _ = h.svc.loginPerAccount.Allow("alice") {
			}
		}
	}

	var body []byte
	switch {
	case c.Request.RawBody != nil:
		body = []byte(*c.Request.RawBody)
	case c.Request.RawBodyBytes > 0:
		body = bytes.Repeat([]byte("a"), c.Request.RawBodyBytes)
	case len(c.Request.Body) > 0:
		body = c.Request.Body
	}
	req := httptest.NewRequest(c.Request.Method, c.Request.Path, bytes.NewReader(body))
	req.Host = testHost
	for k, v := range c.Request.Headers {
		req.Header.Set(k, strings.ReplaceAll(v, fixtureToken, token))
	}
	rec := httptest.NewRecorder()
	h.h.ServeHTTP(rec, req)

	if rec.Code != c.Response.Status {
		t.Fatalf("status %d, want %d (%s)", rec.Code, c.Response.Status, rec.Body.String())
	}
	if rec.Header().Get("X-Request-ID") == "" {
		t.Error("no X-Request-ID")
	}
	if len(c.Response.Body) > 0 {
		var got, want any
		if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
			t.Fatalf("body %q: %v", rec.Body.String(), err)
		}
		_ = json.Unmarshal(c.Response.Body, &want)
		g, _ := json.Marshal(got)
		w, _ := json.Marshal(want)
		if !bytes.Equal(g, w) {
			t.Errorf("body\n got %s\nwant %s", g, w)
		}
	} else if rec.Body.Len() != 0 {
		t.Errorf("unexpected body %q", rec.Body.String())
	}
	for k, v := range c.Response.Headers {
		got := rec.Header().Get(k)
		switch k {
		case "Set-Cookie":
			wantC, err := http.ParseSetCookie(v)
			if err != nil {
				t.Fatal(err)
			}
			gotC := setCookie(t, rec)
			if gotC == nil {
				t.Fatal("no Set-Cookie")
			}
			if gotC.Name != wantC.Name || gotC.MaxAge != wantC.MaxAge || gotC.HttpOnly != wantC.HttpOnly ||
				gotC.Secure != wantC.Secure || gotC.SameSite != wantC.SameSite || gotC.Path != wantC.Path ||
				(gotC.Value == "") != (wantC.Value == "") {
				t.Errorf("Set-Cookie %q, want the shape of %q", got, v)
			}
		case "Retry-After":
			if n, err := strconv.Atoi(got); err != nil || n < 1 || n > int((time.Minute).Seconds()) {
				t.Errorf("Retry-After %q", got)
			}
		default:
			if got != v {
				t.Errorf("%s = %q, want %q", k, got, v)
			}
		}
	}
	if c.Response.Headers["Set-Cookie"] == "" && len(rec.Header().Values("Set-Cookie")) != 0 {
		t.Errorf("unexpected Set-Cookie %v", rec.Header().Values("Set-Cookie"))
	}
}
