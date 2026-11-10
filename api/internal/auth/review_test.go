package auth

import (
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// Malformed requests must not spend the global login/signup budget.
func TestMalformedRequestsDoNotSpendGlobalCaps(t *testing.T) {
	h := newHarness(t, app.SignupOpen, func(o *options) {
		o.LoginGlobal = ratelimit.Config{Every: time.Hour, Burst: 3}
		o.SignupGlobal = ratelimit.Config{Every: time.Hour, Burst: 3}
	})
	for i := 0; i < 100; i++ {
		for _, path := range []string{"/api/auth/login", "/api/auth/signup"} {
			for _, body := range []string{`{not json`, `{"username":"x"}`, strings.Repeat("a", 65537)} {
				if rec := h.do("POST", path, body, ""); rec.Code == 429 {
					t.Fatalf("malformed request %d to %s was rate limited", i, path)
				}
			}
		}
	}
	if rec := h.do("POST", "/api/auth/signup", signupBody{"alice", goodPass, ""}, ""); rec.Code != 201 {
		t.Fatalf("valid signup after malformed flood: %d %s", rec.Code, rec.Body.String())
	}
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""); rec.Code != 200 {
		t.Fatalf("valid login after malformed flood: %d %s", rec.Code, rec.Body.String())
	}
	// Well-formed requests still spend it.
	for i := 0; i < 2; i++ {
		h.do("POST", "/api/auth/login", loginBody{"user" + strconv.Itoa(i), "whatever!!"}, "")
	}
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""); rec.Code != 429 {
		t.Fatalf("global login cap not enforced: %d", rec.Code)
	}
}

// A presented but dead session cookie is cleared on the 401.
func TestInvalidSessionCookieIsCleared(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	h.do("POST", "/api/auth/logout", nil, token)
	for _, route := range [][2]string{{"GET", "/api/me"}, {"POST", "/api/auth/logout"}, {"DELETE", "/api/me"}} {
		for _, cookie := range []string{token, "garbage"} {
			rec := h.do(route[0], route[1], nil, cookie)
			c := setCookie(t, rec)
			if rec.Code != 401 || c == nil || c.Name != CookieName || c.Value != "" || c.MaxAge != -1 || !c.Secure || !c.HttpOnly {
				t.Errorf("%v with dead cookie: %d %v", route, rec.Code, rec.Header().Values("Set-Cookie"))
			}
		}
		// No cookie, nothing to clear.
		if rec := h.do(route[0], route[1], nil, ""); rec.Code != 401 || len(rec.Header().Values("Set-Cookie")) != 0 {
			t.Errorf("%v without cookie: %d %v", route, rec.Code, rec.Header().Values("Set-Cookie"))
		}
	}
}

// A corrupt stored hash is logged and answered like any failed login.
func TestCorruptStoredHashIsInvalidCredentials(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	h.mustSignup("alice", goodPass)
	if _, err := h.db.Writer.Exec("UPDATE users SET pw_hash = 'garbage' WHERE username = 'alice'"); err != nil {
		t.Fatal(err)
	}
	h.hasher.reset()
	rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, "")
	if rec.Code != 401 || errOf(t, rec) != (apperr.BodyError{Code: apperr.CodeInvalidCredentials, Message: "invalid username or password"}) {
		t.Fatalf("corrupt hash: %d %s", rec.Code, rec.Body.String())
	}
	if !strings.Contains(h.logs.String(), "stored password hash") {
		t.Fatalf("corrupt hash not logged: %s", h.logs.String())
	}
	if strings.Contains(h.logs.String(), goodPass) {
		t.Fatal("password logged")
	}
}

// Guard: the key comparison must stay constant-time.
func TestPasswordUsesConstantTimeCompare(t *testing.T) {
	src, err := os.ReadFile("password.go")
	if err != nil {
		t.Fatal(err)
	}
	s := string(src)
	if !regexp.MustCompile(`subtle\.ConstantTimeCompare\(got, want\) == 1`).MatchString(s) {
		t.Fatal("password.go no longer compares keys with subtle.ConstantTimeCompare")
	}
	for _, bad := range []string{"bytes.Equal", "reflect.DeepEqual", "string(got) ==", "string(want) =="} {
		if strings.Contains(s, bad) {
			t.Fatalf("password.go uses %s", bad)
		}
	}
}

func TestPHCVersionMustBeExactly19(t *testing.T) {
	h := NewArgon2id(fastParams, MaxConcurrentHashes)
	good, err := h.Hash(t.Context(), "password123")
	if err != nil {
		t.Fatal(err)
	}
	for _, v := range []string{"v=19x", "v=019", "v=19 ", "v=+19", "V=19", "v=0x13"} {
		bad := strings.Replace(good, "v=19", v, 1)
		if ok, err := h.Verify(t.Context(), "password123", bad); ok || err == nil {
			t.Errorf("version %q accepted", v)
		}
	}
}
