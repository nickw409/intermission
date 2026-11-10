package httpx

import (
	"log/slog"
	"net/http"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Register adds httpx-owned routes (e.g. /healthz) to mux.
//
// TODO(workstream I1): register /healthz.
func Register(mux *http.ServeMux, d *app.Deps) {}

// Wrap applies the middleware chain that runs around every route.
//
// Placeholder (workstream I1 extends it): assigns a request ID (reusing a
// valid incoming X-Request-ID), echoes it in the response, stores it in the
// request context, and writes one JSON access-log line per request with
// request_id, method, route, status and latency. It never logs bodies,
// query strings, cookies or headers.
func Wrap(h http.Handler, d *app.Deps) http.Handler {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		id := r.Header.Get(reqid.Header)
		if !reqid.Valid(id) {
			id = reqid.New()
		}
		w.Header().Set(reqid.Header, id)
		r = r.WithContext(reqid.WithID(r.Context(), id))
		sw := &statusWriter{ResponseWriter: w}
		h.ServeHTTP(sw, r)
		route := r.Pattern // set by http.ServeMux on match
		if route == "" {
			route = "unmatched"
		}
		logger.LogAttrs(r.Context(), slog.LevelInfo, "request",
			slog.String("request_id", id),
			slog.String("method", r.Method),
			slog.String("route", route),
			slog.Int("status", sw.status()),
			slog.Float64("latency_ms", float64(time.Since(start).Microseconds())/1000),
		)
	})
}

type statusWriter struct {
	http.ResponseWriter
	code int
}

func (w *statusWriter) WriteHeader(code int) {
	if w.code == 0 {
		w.code = code
	}
	w.ResponseWriter.WriteHeader(code)
}

func (w *statusWriter) Write(b []byte) (int, error) {
	if w.code == 0 {
		w.code = http.StatusOK
	}
	return w.ResponseWriter.Write(b)
}

func (w *statusWriter) status() int {
	if w.code == 0 {
		return http.StatusOK
	}
	return w.code
}

// Unwrap lets http.ResponseController reach the underlying writer.
func (w *statusWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }
