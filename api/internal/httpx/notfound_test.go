package httpx

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// Security audit SA3-5: the mux's own 404 and 405 answers under /api/ use
// the contract's Error JSON, like every other API error.
func TestUnmatchedAPIRoutesAnswerErrorJSON(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/x", func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusNoContent) })
	mux.HandleFunc("GET /api/item/{id}", func(w http.ResponseWriter, r *http.Request) {
		apperr.WriteError(w, http.StatusNotFound, apperr.CodeNotFound, "movie not found")
	})
	h := Wrap(mux, testDeps(nil))

	for _, c := range []struct {
		method, path string
		status       int
		code         apperr.Code
		allow        string
	}{
		{"GET", "/api/nope", 404, apperr.CodeNotFound, ""},
		{"GET", "/api/x/deeper", 404, apperr.CodeNotFound, ""},
		{"DELETE", "/api/x", 405, apperr.CodeBadRequest, "GET, HEAD"},
		{"POST", "/api/x", 405, apperr.CodeBadRequest, "GET, HEAD"},
	} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, sameSiteReq(c.method, c.path, nil))
		if rec.Code != c.status {
			t.Errorf("%s %s: status %d, want %d", c.method, c.path, rec.Code, c.status)
			continue
		}
		if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
			t.Errorf("%s %s: Content-Type %q, body %q", c.method, c.path, ct, rec.Body.String())
			continue
		}
		if e := errBody(t, rec); e.Code != c.code || e.Message == "" {
			t.Errorf("%s %s: error %+v, want code %s", c.method, c.path, e, c.code)
		}
		if got := rec.Header().Get("Allow"); got != c.allow {
			t.Errorf("%s %s: Allow %q, want %q", c.method, c.path, got, c.allow)
		}
	}

	// A handler's own 404 is left exactly as written.
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, sameSiteReq("GET", "/api/item/5", nil))
	if e := errBody(t, rec); rec.Code != 404 || e.Message != "movie not found" {
		t.Errorf("handler 404 rewritten: %d %+v", rec.Code, e)
	}
	// Outside /api/ nothing changes.
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, sameSiteReq("GET", "/elsewhere", nil))
	if ct := rec.Header().Get("Content-Type"); rec.Code != 404 || strings.HasPrefix(ct, "application/json") {
		t.Errorf("non-API 404 changed: %d %q", rec.Code, ct)
	}
}
