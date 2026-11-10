package httpx

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
)

const site = "https://intermission.example"

func testDeps(logs io.Writer) *app.Deps {
	if logs == nil {
		logs = io.Discard
	}
	return &app.Deps{
		Config: app.Config{SiteOrigin: site},
		Logger: slog.New(slog.NewJSONHandler(logs, nil)),
	}
}

func errBody(t *testing.T, rec *httptest.ResponseRecorder) apperr.BodyError {
	t.Helper()
	var b apperr.Body
	if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil {
		t.Fatalf("error body %q: %v", rec.Body.String(), err)
	}
	return b.Error
}

// sameSiteReq builds a request that passes every CSRF check.
func sameSiteReq(method, path string, body io.Reader) *http.Request {
	req := httptest.NewRequest(method, path, body)
	req.Host = "intermission.example"
	req.Header.Set("Origin", site)
	req.Header.Set("X-Requested-With", "recs")
	return req
}

func TestCSRFOnEveryMutatingMethod(t *testing.T) {
	reached := 0
	mux := http.NewServeMux()
	mux.HandleFunc("/api/x", func(w http.ResponseWriter, r *http.Request) { reached++; w.WriteHeader(http.StatusNoContent) })
	h := Wrap(mux, testDeps(nil))

	cases := []struct {
		name   string
		mutate func(*http.Request)
	}{
		{"no Origin", func(r *http.Request) { r.Header.Del("Origin") }},
		{"foreign Origin", func(r *http.Request) { r.Header.Set("Origin", "https://evil.example") }},
		{"Origin with other port", func(r *http.Request) { r.Header.Set("Origin", site+":8443") }},
		{"Origin null", func(r *http.Request) { r.Header.Set("Origin", "null") }},
		{"no X-Requested-With", func(r *http.Request) { r.Header.Del("X-Requested-With") }},
		{"wrong X-Requested-With", func(r *http.Request) { r.Header.Set("X-Requested-With", "XMLHttpRequest") }},
		// These two pass our own checks and are stopped by http.CrossOriginProtection.
		{"Sec-Fetch-Site cross-site", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "cross-site") }},
		{"Origin does not match Host", func(r *http.Request) { r.Host = "other.example" }},
	}
	for _, method := range []string{"POST", "PUT", "PATCH", "DELETE"} {
		for _, c := range cases {
			req := sameSiteReq(method, "/api/x", nil)
			c.mutate(req)
			rec := httptest.NewRecorder()
			h.ServeHTTP(rec, req)
			if rec.Code != http.StatusForbidden {
				t.Errorf("%s %s: status %d", method, c.name, rec.Code)
				continue
			}
			if e := errBody(t, rec); e.Code != apperr.CodeCSRFRejected || e.Message != "missing or invalid X-Requested-With or Origin" {
				t.Errorf("%s %s: body %+v", method, c.name, e)
			}
		}
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, sameSiteReq(method, "/api/x", nil))
		if rec.Code != http.StatusNoContent {
			t.Errorf("%s valid: status %d", method, rec.Code)
		}
		req := sameSiteReq(method, "/api/x", nil)
		req.Header.Set("Sec-Fetch-Site", "same-origin")
		rec = httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		if rec.Code != http.StatusNoContent {
			t.Errorf("%s same-origin fetch: status %d", method, rec.Code)
		}
	}
	if reached != 8 {
		t.Fatalf("handler reached %d times, want 8", reached)
	}
	// Safe methods need no CSRF headers.
	for _, method := range []string{"GET", "HEAD", "OPTIONS"} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(method, "/api/x", nil))
		if rec.Code != http.StatusNoContent {
			t.Errorf("%s: status %d", method, rec.Code)
		}
	}
}

// With no configured site origin every mutating request is refused.
func TestCSRFFailsClosedWithoutSiteOrigin(t *testing.T) {
	d := testDeps(nil)
	d.Config.SiteOrigin = ""
	h := Wrap(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}), d)
	req := sameSiteReq("POST", "/api/x", nil)
	req.Header.Set("Origin", "")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status %d", rec.Code)
	}
}

type payload struct {
	Name string `json:"name"`
	N    int    `json:"n"`
}

func decodeVia(t *testing.T, req *http.Request) (*httptest.ResponseRecorder, payload) {
	t.Helper()
	var got payload
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/x", func(w http.ResponseWriter, r *http.Request) {
		if err := DecodeJSON(r, &got); err != nil {
			apperr.Write(w, err)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	rec := httptest.NewRecorder()
	Wrap(mux, testDeps(nil)).ServeHTTP(rec, req)
	return rec, got
}

func TestDecodeJSON(t *testing.T) {
	cases := []struct {
		name   string
		body   string
		status int
		code   apperr.Code
		msg    string
	}{
		{"ok", `{"name":"a","n":2}`, 204, "", ""},
		{"malformed", `{not json`, 400, apperr.CodeInvalidJSON, "request body is not valid JSON"},
		{"empty", ``, 400, apperr.CodeInvalidJSON, "request body is not valid JSON"},
		{"truncated", `{"name":`, 400, apperr.CodeInvalidJSON, "request body is not valid JSON"},
		{"trailing data", `{"name":"a"} {"name":"b"}`, 400, apperr.CodeInvalidJSON, "request body is not valid JSON"},
		{"unknown field", `{"name":"a","admin":true}`, 400, apperr.CodeBadRequest, "request body has an unknown field"},
		{"wrong type", `{"name":5}`, 400, apperr.CodeBadRequest, "request body has a field of the wrong type"},
		{"not an object", `[1,2]`, 400, apperr.CodeBadRequest, "request body has a field of the wrong type"},
		{"too large", `{"name":"` + strings.Repeat("a", 64<<10) + `"}`, 413, apperr.CodeBodyTooLarge, "request body exceeds 64 KB"},
	}
	for _, c := range cases {
		rec, _ := decodeVia(t, sameSiteReq("POST", "/api/x", strings.NewReader(c.body)))
		if rec.Code != c.status {
			t.Errorf("%s: status %d, want %d (%s)", c.name, rec.Code, c.status, rec.Body.String())
			continue
		}
		if c.code != "" {
			if e := errBody(t, rec); e.Code != c.code || e.Message != c.msg {
				t.Errorf("%s: %+v", c.name, e)
			}
		}
	}
}

func TestBodyLimitBoundary(t *testing.T) {
	// Exactly 64 KB is accepted; one byte more is not.
	body := `{"name":"a"}`
	exact := body + strings.Repeat(" ", MaxBodyBytes-len(body))
	if rec, got := decodeVia(t, sameSiteReq("POST", "/api/x", strings.NewReader(exact))); rec.Code != 204 || got.Name != "a" {
		t.Fatalf("64 KB body: %d %s", rec.Code, rec.Body.String())
	}
	if rec, _ := decodeVia(t, sameSiteReq("POST", "/api/x", strings.NewReader(exact+" "))); rec.Code != 413 {
		t.Fatalf("64 KB + 1: %d", rec.Code)
	}
	// Without a Content-Length (chunked) the cap still applies while reading.
	req := sameSiteReq("POST", "/api/x", io.MultiReader(strings.NewReader(exact), strings.NewReader("  ")))
	req.ContentLength = -1
	if rec, _ := decodeVia(t, req); rec.Code != 413 {
		t.Fatalf("chunked oversize: %d", rec.Code)
	}
	// 65537 bytes of junk (the contract fixture) is 413, not invalid_json.
	if rec, _ := decodeVia(t, sameSiteReq("POST", "/api/x", bytes.NewReader(make([]byte, 65537)))); rec.Code != 413 {
		t.Fatalf("junk: %d", rec.Code)
	}
}

func TestRecoverWritesInternalError(t *testing.T) {
	var logs bytes.Buffer
	mux := http.NewServeMux()
	mux.HandleFunc("GET /boom", func(w http.ResponseWriter, r *http.Request) { panic("kaboom") })
	rec := httptest.NewRecorder()
	Wrap(mux, testDeps(&logs)).ServeHTTP(rec, httptest.NewRequest("GET", "/boom", nil))
	if rec.Code != 500 {
		t.Fatalf("status %d", rec.Code)
	}
	if e := errBody(t, rec); e.Code != apperr.CodeInternal || strings.Contains(e.Message, "kaboom") {
		t.Fatalf("body %+v", e)
	}
	if !strings.Contains(logs.String(), "panic") || !strings.Contains(logs.String(), `"status":500`) {
		t.Fatalf("logs %s", logs.String())
	}
}

func TestAccessLogHasUserIDAndNoSecrets(t *testing.T) {
	var logs bytes.Buffer
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/x", func(w http.ResponseWriter, r *http.Request) {
		SetUserID(r.Context(), 42)
		_, _ = io.ReadAll(r.Body)
		w.WriteHeader(http.StatusNoContent)
	})
	req := sameSiteReq("POST", "/api/x?token=qsecret", strings.NewReader(`{"password":"bodysecret"}`))
	req.Header.Set("Cookie", "session=cookiesecret")
	req.Header.Set("Authorization", "Bearer authsecret")
	rec := httptest.NewRecorder()
	Wrap(mux, testDeps(&logs)).ServeHTTP(rec, req)
	var line map[string]any
	if err := json.Unmarshal(logs.Bytes(), &line); err != nil {
		t.Fatal(err)
	}
	for _, k := range []string{"request_id", "method", "route", "status", "latency_ms"} {
		if _, ok := line[k]; !ok {
			t.Errorf("log missing %s: %v", k, line)
		}
	}
	if line["user_id"] != float64(42) {
		t.Errorf("user_id = %v", line["user_id"])
	}
	for _, s := range []string{"qsecret", "bodysecret", "cookiesecret", "authsecret"} {
		if strings.Contains(logs.String(), s) {
			t.Errorf("%s logged: %s", s, logs.String())
		}
	}
}

func TestHealthz(t *testing.T) {
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	d := testDeps(nil)
	d.Model = f
	mux := http.NewServeMux()
	Register(mux, d)
	h := Wrap(mux, d)

	get := func() *httptest.ResponseRecorder {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest("GET", "/healthz", nil))
		return rec
	}
	if rec := get(); rec.Code != 200 || strings.TrimSpace(rec.Body.String()) != `{"status":"ok"}` {
		t.Fatalf("up: %d %s", rec.Code, rec.Body.String())
	}
	f.SetDown(true)
	if rec := get(); rec.Code != 503 || strings.TrimSpace(rec.Body.String()) != `{"status":"degraded"}` {
		t.Fatalf("down: %d %s", rec.Code, rec.Body.String())
	}
}
