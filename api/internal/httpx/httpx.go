package httpx

import (
	"context"
	"log/slog"
	"net/http"
	"runtime/debug"
	"strings"
	"sync/atomic"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// MaxBodyBytes is the request body cap (spec: "request bodies up to 64 KB").
const MaxBodyBytes = 64 << 10

// HealthTimeout bounds the recs-model check in /healthz.
const HealthTimeout = time.Second

// Register adds httpx-owned routes (/healthz) to mux.
func Register(mux *http.ServeMux, d *app.Deps) {
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		if healthy(r.Context(), d) {
			apperr.WriteJSON(w, http.StatusOK, map[string]string{"status": "ok"})
			return
		}
		apperr.WriteJSON(w, http.StatusServiceUnavailable, map[string]string{"status": "degraded"})
	})
}

// healthy reports whether app.db answers and recs-model is up. It reveals
// nothing beyond that.
func healthy(ctx context.Context, d *app.Deps) bool {
	ctx, cancel := context.WithTimeout(ctx, HealthTimeout)
	defer cancel()
	if d.DB != nil && d.DB.Reader != nil {
		if err := d.DB.Reader.PingContext(ctx); err != nil {
			return false
		}
	}
	if d.Model == nil {
		return false
	}
	return d.Model.Healthz(ctx) == nil
}

// Wrap applies the middleware chain that runs around every route, outermost
// first:
//
//  1. request ID: reuse a valid incoming X-Request-ID or mint one; echo it in
//     the response and store it in the request context;
//  2. access log: one JSON line per request with request_id, method, route,
//     status, latency_ms and user_id (when a session was found). It never
//     logs bodies, query strings, cookies or other headers;
//  3. panic recovery: 500 internal, details only in the log;
//  4. CSRF for every mutating method (see csrf.go);
//  5. the 64 KB body cap (http.MaxBytesReader; DecodeJSON maps it to 413).
func Wrap(h http.Handler, d *app.Deps) http.Handler {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	inner := CSRF(d.Config.SiteOrigin)(limitBody(h))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		id := r.Header.Get(reqid.Header)
		if !reqid.Valid(id) {
			id = reqid.New()
		}
		w.Header().Set(reqid.Header, id)
		info := &requestInfo{}
		ctx := reqid.WithID(r.Context(), id)
		ctx = context.WithValue(ctx, infoKey{}, info)
		r = r.WithContext(ctx)
		sw := &statusWriter{ResponseWriter: w}

		func() {
			defer func() {
				if v := recover(); v != nil {
					if v == http.ErrAbortHandler {
						panic(v)
					}
					logger.LogAttrs(ctx, slog.LevelError, "panic",
						slog.String("request_id", id),
						slog.Any("value", v),
						slog.String("stack", string(debug.Stack())),
					)
					if sw.code == 0 {
						apperr.WriteError(sw, http.StatusInternalServerError, apperr.CodeInternal, "internal error")
					}
				}
			}()
			var out http.ResponseWriter = sw
			if unmatchedAPI(h, r) {
				out = &apiFallbackWriter{ResponseWriter: sw}
			}
			inner.ServeHTTP(out, r)
		}()

		route := r.Pattern // set by http.ServeMux on match
		if route == "" {
			route = "unmatched"
		}
		attrs := []slog.Attr{
			slog.String("request_id", id),
			slog.String("method", r.Method),
			slog.String("route", route),
			slog.Int("status", sw.status()),
			slog.Float64("latency_ms", float64(time.Since(start).Microseconds())/1000),
		}
		if uid := info.userID.Load(); uid != 0 {
			attrs = append(attrs, slog.Int64("user_id", uid))
		}
		logger.LogAttrs(ctx, slog.LevelInfo, "request", attrs...)
	})
}

// unmatchedAPI reports whether r is under /api/ and matches no registered
// pattern of h (an *http.ServeMux), so the mux itself will answer 404 or
// 405 in plain text.
func unmatchedAPI(h http.Handler, r *http.Request) bool {
	mux, ok := h.(*http.ServeMux)
	if !ok || !strings.HasPrefix(r.URL.Path, "/api/") {
		return false
	}
	_, pattern := mux.Handler(r)
	return pattern == ""
}

// apiFallbackWriter turns the mux's plain-text 404 and 405 into the
// contract's Error JSON (security audit SA3-5): 404 not_found, and 405
// with code bad_request (the contract has no method_not_allowed code) and
// the mux's Allow header kept. Any other response passes through.
type apiFallbackWriter struct {
	http.ResponseWriter
	replaced bool
}

func (w *apiFallbackWriter) WriteHeader(code int) {
	switch code {
	case http.StatusNotFound:
		w.replaced = true
		apperr.WriteError(w.ResponseWriter, code, apperr.CodeNotFound, "not found")
	case http.StatusMethodNotAllowed:
		w.replaced = true
		apperr.WriteError(w.ResponseWriter, code, apperr.CodeBadRequest, "method not allowed")
	default:
		w.ResponseWriter.WriteHeader(code)
	}
}

func (w *apiFallbackWriter) Write(b []byte) (int, error) {
	if w.replaced {
		return len(b), nil // the mux's text body; the JSON is already out
	}
	return w.ResponseWriter.Write(b)
}

// Unwrap lets http.ResponseController reach the underlying writer.
func (w *apiFallbackWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

// requestInfo is filled in by handlers for the access log.
type requestInfo struct {
	userID atomic.Int64
}

type infoKey struct{}

// SetUserID records the authenticated user for the access log. auth's
// session middleware calls it; ctx must come from a request inside Wrap.
func SetUserID(ctx context.Context, id int64) {
	if info, ok := ctx.Value(infoKey{}).(*requestInfo); ok {
		info.userID.Store(id)
	}
}

func limitBody(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Body != nil && r.Body != http.NoBody {
			r.Body = http.MaxBytesReader(w, r.Body, MaxBodyBytes)
		}
		h.ServeHTTP(w, r)
	})
}

// isSafeMethod reports whether method never changes state.
func isSafeMethod(method string) bool {
	switch strings.ToUpper(method) {
	case http.MethodGet, http.MethodHead, http.MethodOptions:
		return true
	}
	return false
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
