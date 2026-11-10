package auth

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

const (
	testSite = "https://intermission.example"
	testHost = "intermission.example"
	goodPass = "correct horse battery"
)

type clock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *clock) Now() time.Time { c.mu.Lock(); defer c.mu.Unlock(); return c.t }
func (c *clock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.t = c.t.Add(d)
}

// countingHasher wraps a real (cheap) Argon2id and counts calls.
type countingHasher struct {
	inner          Hasher
	hashes, checks atomic.Int64
}

func (c *countingHasher) Hash(ctx context.Context, pw string) (string, error) {
	c.hashes.Add(1)
	return c.inner.Hash(ctx, pw)
}

func (c *countingHasher) Verify(ctx context.Context, pw, phc string) (bool, error) {
	c.checks.Add(1)
	return c.inner.Verify(ctx, pw, phc)
}

func (c *countingHasher) reset() { c.hashes.Store(0); c.checks.Store(0) }

type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Write(p) }
func (s *syncBuffer) String() string              { s.mu.Lock(); defer s.mu.Unlock(); return s.b.String() }

type harness struct {
	t      *testing.T
	h      http.Handler
	db     *db.DB
	svc    *service
	clock  *clock
	hasher *countingHasher
	logs   *syncBuffer
}

// generous limits so tests that aren't about rate limiting never hit them.
var unlimited = ratelimit.Config{Every: time.Microsecond, Burst: 1 << 20}

func newHarness(t *testing.T, mode app.SignupMode, mutate ...func(*options)) *harness {
	t.Helper()
	ctx := context.Background()
	database, err := db.Open(ctx, filepath.Join(t.TempDir(), "app.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = database.Close() })
	if err := database.Migrate(ctx, slog.New(slog.NewTextHandler(io.Discard, nil))); err != nil {
		t.Fatal(err)
	}
	logs := &syncBuffer{}
	d := &app.Deps{
		Config: app.Config{SiteOrigin: testSite, SignupMode: mode},
		Logger: slog.New(slog.NewJSONHandler(logs, nil)),
		DB:     database,
	}
	clk := &clock{t: time.Unix(1794256680, 0)}
	hasher := &countingHasher{inner: NewArgon2id(fastParams, MaxConcurrentHashes)}
	o := options{
		Hasher:          hasher,
		Now:             clk.Now,
		LoginPerAccount: unlimited,
		LoginGlobal:     unlimited,
		SignupGlobal:    unlimited,
	}
	for _, m := range mutate {
		m(&o)
	}
	svc, err := newService(d, o)
	if err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	svc.register(mux)
	hasher.reset() // newService hashes the dummy password once
	return &harness{t: t, h: httpx.Wrap(mux, d), db: database, svc: svc, clock: clk, hasher: hasher, logs: logs}
}

// do sends a same-origin request (passing CSRF) with an optional JSON body
// (a string is sent raw) and session token.
func (h *harness) do(method, path string, body any, token string, mutate ...func(*http.Request)) *httptest.ResponseRecorder {
	h.t.Helper()
	var r io.Reader
	switch b := body.(type) {
	case nil:
	case string:
		r = strings.NewReader(b)
	default:
		buf, err := json.Marshal(b)
		if err != nil {
			h.t.Fatal(err)
		}
		r = bytes.NewReader(buf)
	}
	req := httptest.NewRequest(method, path, r)
	req.Host = testHost
	req.Header.Set("Origin", testSite)
	req.Header.Set("X-Requested-With", "recs")
	if token != "" {
		req.AddCookie(&http.Cookie{Name: CookieName, Value: token})
	}
	for _, m := range mutate {
		m(req)
	}
	rec := httptest.NewRecorder()
	h.h.ServeHTTP(rec, req)
	return rec
}

func (h *harness) mintInvite(uses int, expires time.Duration) string {
	h.t.Helper()
	s := invites.NewWithClock(h.db, h.clock.Now)
	code, err := s.Create(context.Background(), uses, expires, "test")
	if err != nil {
		h.t.Fatal(err)
	}
	return code
}

type signupBody struct {
	Username   string `json:"username"`
	Password   string `json:"password"`
	InviteCode string `json:"invite_code,omitempty"`
}

type loginBody struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// mustSignup creates a user (minting an invite) and returns the session token.
func (h *harness) mustSignup(username, password string) string {
	h.t.Helper()
	rec := h.do("POST", "/api/auth/signup", signupBody{username, password, h.mintInvite(1, 0)}, "")
	if rec.Code != http.StatusCreated {
		h.t.Fatalf("signup %s: %d %s", username, rec.Code, rec.Body.String())
	}
	return sessionToken(h.t, rec)
}

func (h *harness) count(query string, args ...any) int {
	h.t.Helper()
	var n int
	if err := h.db.Reader.QueryRow(query, args...).Scan(&n); err != nil {
		h.t.Fatal(err)
	}
	return n
}

// sessionToken returns the session cookie value set by rec.
func sessionToken(t *testing.T, rec *httptest.ResponseRecorder) string {
	t.Helper()
	c := setCookie(t, rec)
	if c == nil || c.Value == "" {
		t.Fatalf("no session cookie in %v", rec.Header().Values("Set-Cookie"))
	}
	return c.Value
}

func setCookie(t *testing.T, rec *httptest.ResponseRecorder) *http.Cookie {
	t.Helper()
	vals := rec.Header().Values("Set-Cookie")
	if len(vals) == 0 {
		return nil
	}
	if len(vals) > 1 {
		t.Fatalf("multiple Set-Cookie: %v", vals)
	}
	c, err := http.ParseSetCookie(vals[0])
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func errOf(t *testing.T, rec *httptest.ResponseRecorder) apperr.BodyError {
	t.Helper()
	var b apperr.Body
	if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil {
		t.Fatalf("not an error body: %q", rec.Body.String())
	}
	return b.Error
}

func meOf(t *testing.T, rec *httptest.ResponseRecorder) Me {
	t.Helper()
	var m Me
	dec := json.NewDecoder(bytes.NewReader(rec.Body.Bytes()))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&m); err != nil {
		t.Fatalf("not a Me body: %q", rec.Body.String())
	}
	return m
}
