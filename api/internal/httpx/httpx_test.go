package httpx

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/reqid"
)

func TestWrapRequestIDAndAccessLog(t *testing.T) {
	var logs bytes.Buffer
	d := &app.Deps{Logger: slog.New(slog.NewJSONHandler(&logs, nil))}
	mux := http.NewServeMux()
	var seen string
	mux.HandleFunc("GET /api/things/{id}", func(w http.ResponseWriter, r *http.Request) {
		seen = reqid.FromContext(r.Context())
		w.WriteHeader(http.StatusTeapot)
	})
	h := Wrap(mux, d)

	req := httptest.NewRequest("GET", "/api/things/7?secret=shh", nil)
	req.Header.Set("X-Request-ID", "incoming-1")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Header().Get("X-Request-ID") != "incoming-1" || seen != "incoming-1" {
		t.Fatalf("request id: header=%q ctx=%q", rec.Header().Get("X-Request-ID"), seen)
	}
	var line map[string]any
	if err := json.Unmarshal(logs.Bytes(), &line); err != nil {
		t.Fatalf("log line: %v: %s", err, logs.String())
	}
	if line["route"] != "GET /api/things/{id}" || line["status"] != float64(418) || line["request_id"] != "incoming-1" {
		t.Fatalf("log line = %v", line)
	}
	if strings.Contains(logs.String(), "shh") {
		t.Fatal("query string logged")
	}

	// Invalid incoming IDs are replaced.
	logs.Reset()
	req = httptest.NewRequest("GET", "/nope", nil)
	req.Header.Set("X-Request-ID", "bad id\n")
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if got := rec.Header().Get("X-Request-ID"); !reqid.Valid(got) || got == "bad id\n" {
		t.Fatalf("replacement id = %q", got)
	}
	if !strings.Contains(logs.String(), `"route":"unmatched"`) || !strings.Contains(logs.String(), `"status":404`) {
		t.Fatalf("log = %s", logs.String())
	}
}
