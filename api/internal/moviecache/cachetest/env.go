package cachetest

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/contractsdir"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache"
)

// Site is the origin used by Env (CSRF checks pass for requests from it).
const (
	Site = "https://intermission.example"
	Host = "intermission.example"
)

// FixtureToken is the placeholder session token in contracts/fixtures/public.
const FixtureToken = "fixture-session-token"

// SyncBuffer is a goroutine-safe log sink.
type SyncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *SyncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *SyncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// Env is a recs-api handler stack (httpx.Wrap around a mux) over a fresh
// database, with the replay fake as recs-model.
type Env struct {
	T       testing.TB
	DB      *db.DB
	Fake    *fake.Fake
	Deps    *app.Deps
	Handler http.Handler
	Logs    *SyncBuffer
}

// NewEnv builds an Env and lets register add routes. model, if non-nil,
// replaces the fake as the client (the fake is still created).
func NewEnv(t testing.TB, model modelclient.Client, register ...func(*http.ServeMux, *app.Deps)) *Env {
	t.Helper()
	d := OpenDB(t)
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	if model == nil {
		model = f
	}
	logs := &SyncBuffer{}
	deps := &app.Deps{
		Config: app.Config{SiteOrigin: Site, SignupMode: app.SignupInvite},
		Logger: slog.New(slog.NewJSONHandler(logs, &slog.HandlerOptions{Level: slog.LevelDebug})),
		DB:     d,
		Model:  model,
	}
	deps.Movies = moviecache.New(d, model, moviecache.WithLogger(deps.Logger))
	mux := http.NewServeMux()
	for _, r := range register {
		r(mux, deps)
	}
	return &Env{T: t, DB: d, Fake: f, Deps: deps, Handler: httpx.Wrap(mux, deps), Logs: logs}
}

// Do sends a same-origin request (passing CSRF). body may be nil, a string
// (sent raw) or a value to JSON-encode. token, if set, is the session cookie.
func (e *Env) Do(method, path string, body any, token string) *httptest.ResponseRecorder {
	e.T.Helper()
	var r io.Reader
	switch b := body.(type) {
	case nil:
	case string:
		r = strings.NewReader(b)
	default:
		buf, err := json.Marshal(b)
		if err != nil {
			e.T.Fatal(err)
		}
		r = bytes.NewReader(buf)
	}
	req := httptest.NewRequest(method, path, r)
	req.Host = Host
	req.Header.Set("Origin", Site)
	req.Header.Set("X-Requested-With", "recs")
	if token != "" {
		req.AddCookie(&http.Cookie{Name: "session", Value: token})
	}
	rec := httptest.NewRecorder()
	e.Handler.ServeHTTP(rec, req)
	return rec
}

// ErrorCode returns the error code of an error response, or "".
func ErrorCode(rec *httptest.ResponseRecorder) string {
	var b struct {
		Error struct {
			Code string `json:"code"`
		} `json:"error"`
	}
	_ = json.Unmarshal(rec.Body.Bytes(), &b)
	return b.Error.Code
}

// ---------------------------------------------------------------- fixtures

// Case is one case of a contracts/fixtures/public/<operationId>.json file.
type Case struct {
	Name    string `json:"name"`
	Request struct {
		Method       string            `json:"method"`
		Path         string            `json:"path"`
		Query        map[string]string `json:"query"`
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

// PublicCases loads contracts/fixtures/public/<op>.json.
func PublicCases(t testing.TB, op string) []Case {
	t.Helper()
	dir, err := contractsdir.Fixtures("public")
	if err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(filepath.Join(dir, op+".json"))
	if err != nil {
		t.Fatal(err)
	}
	var ff struct {
		OperationID string `json:"operationId"`
		Cases       []Case `json:"cases"`
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&ff); err != nil {
		t.Fatalf("%s: %v", op, err)
	}
	if ff.OperationID != op || len(ff.Cases) == 0 {
		t.Fatalf("%s: bad fixture file", op)
	}
	return ff.Cases
}

// Replay sends the case's request (with FixtureToken replaced by token) and
// checks status, the exact JSON body, every fixture header and X-Request-ID.
func (e *Env) Replay(c Case, token string) *httptest.ResponseRecorder {
	t := e.T
	t.Helper()
	var body []byte
	switch {
	case c.Request.RawBody != nil:
		body = []byte(*c.Request.RawBody)
	case c.Request.RawBodyBytes > 0:
		body = bytes.Repeat([]byte("a"), c.Request.RawBodyBytes)
	case len(c.Request.Body) > 0:
		body = c.Request.Body
	}
	target := c.Request.Path
	if len(c.Request.Query) > 0 {
		q := url.Values{}
		for k, v := range c.Request.Query {
			q.Set(k, v)
		}
		target += "?" + q.Encode()
	}
	req := httptest.NewRequest(c.Request.Method, target, bytes.NewReader(body))
	req.Host = Host
	for k, v := range c.Request.Headers {
		req.Header.Set(k, strings.ReplaceAll(v, FixtureToken, token))
	}
	rec := httptest.NewRecorder()
	e.Handler.ServeHTTP(rec, req)

	if rec.Code != c.Response.Status {
		t.Fatalf("status %d, want %d (%s)", rec.Code, c.Response.Status, rec.Body.String())
	}
	if rec.Header().Get("X-Request-ID") == "" {
		t.Error("no X-Request-ID")
	}
	for k, v := range c.Response.Headers {
		if got := rec.Header().Get(k); got != v {
			t.Errorf("%s = %q, want %q", k, got, v)
		}
	}
	if len(c.Response.Body) == 0 {
		if rec.Body.Len() != 0 {
			t.Errorf("unexpected body %q", rec.Body.String())
		}
		return rec
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("Content-Type %q", ct)
	}
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
	return rec
}
