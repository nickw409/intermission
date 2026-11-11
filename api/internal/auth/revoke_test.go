package auth

import (
	"context"
	"net/http"
	"testing"

	"github.com/nickw409/intermission/api/internal/app"
)

// RevokeAllSessions (`recs-api admin sessions revoke-all`) logs every user
// out: cookies issued by signup and login stop working at once, and the
// accounts remain, so users can log in again.
func TestRevokeAllSessions(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	alice := h.mustSignup("alice", goodPass)
	bob := h.mustSignup("bob", goodPass)
	for _, tok := range []string{alice, bob} {
		if rec := h.do("GET", "/api/me", nil, tok); rec.Code != http.StatusOK {
			t.Fatalf("before: %d %s", rec.Code, rec.Body.String())
		}
	}

	n, err := RevokeAllSessions(context.Background(), h.db)
	if err != nil {
		t.Fatal(err)
	}
	if n != 2 {
		t.Errorf("revoked %d, want 2", n)
	}
	for _, tok := range []string{alice, bob} {
		if rec := h.do("GET", "/api/me", nil, tok); rec.Code != http.StatusUnauthorized {
			t.Errorf("after: %d, want 401", rec.Code)
		}
	}
	if got := h.count("SELECT count(*) FROM users"); got != 2 {
		t.Errorf("%d users left, want 2", got)
	}
	rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, "")
	if rec.Code != http.StatusOK {
		t.Fatalf("login after revoke: %d %s", rec.Code, rec.Body.String())
	}
	if rec := h.do("GET", "/api/me", nil, sessionToken(t, rec)); rec.Code != http.StatusOK {
		t.Errorf("new session: %d", rec.Code)
	}

	if n, err := RevokeAllSessions(context.Background(), h.db); err != nil || n != 1 {
		t.Errorf("second revoke: %d %v", n, err)
	}
}
