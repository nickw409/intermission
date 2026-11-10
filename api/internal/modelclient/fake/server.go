package fake

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"

	"github.com/nickw409/intermission/api/internal/reqid"
)

// maxBody mirrors recs-model's 1 MiB request body limit.
const maxBody = 1 << 20

// Server is an httptest.Server speaking the recs-model HTTP API from the
// fixtures. Use Fake for the knobs (SetDown, SetLatency, Force, Calls).
type Server struct {
	*httptest.Server
	Fake *Fake
}

// NewServer loads the fixtures (see New) and starts a Server on a loopback
// TCP port. Call Close when done.
func NewServer() (*Server, error) {
	f, err := New()
	if err != nil {
		return nil, err
	}
	return NewServerFor(f), nil
}

// NewServerFor starts a Server backed by f.
func NewServerFor(f *Fake) *Server {
	return &Server{Server: httptest.NewServer(Handler(f)), Fake: f}
}

// Handler returns the http.Handler used by Server, e.g. to serve it on a
// Unix socket listener.
func Handler(f *Fake) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get(reqid.Header)
		if !reqid.Valid(id) {
			id = reqid.New()
		}
		w.Header().Set(reqid.Header, id)

		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBody))
		if err != nil {
			writeErr(w, http.StatusBadRequest, "body_too_large", "request body exceeds 1 MiB")
			return
		}
		opID, ok := route(r.Method, r.URL.Path)
		if !ok {
			writeErr(w, http.StatusNotFound, "not_found", "no such route")
			return
		}
		c, err := f.resolve(r.Context(), opID, r.Method, r.URL.Path, r.URL.Query(), body, r.Header.Get(reqid.Header))
		switch {
		case errors.Is(err, errDown):
			writeErr(w, http.StatusServiceUnavailable, "model_unavailable", "model unavailable")
			return
		case errors.Is(err, context.Canceled), errors.Is(err, context.DeadlineExceeded):
			return // client went away
		case err != nil:
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}

		for k, v := range c.Response.Headers {
			w.Header().Set(k, v)
		}
		respBody := bytes.TrimSpace(c.Response.Body)
		ct := c.Response.Header("Content-Type")
		if len(respBody) > 0 && respBody[0] == '"' && ct != "" && !strings.Contains(ct, "json") {
			// A JSON string body with a non-JSON content type (e.g. /metrics
			// text/plain) is written as raw text.
			var s string
			if json.Unmarshal(respBody, &s) == nil {
				respBody = []byte(s)
			}
		} else if ct == "" && len(respBody) > 0 {
			w.Header().Set("Content-Type", "application/json")
		}
		w.WriteHeader(c.Response.Status)
		_, _ = w.Write(respBody)
	})
}

func writeErr(w http.ResponseWriter, status int, code, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	b, _ := json.Marshal(map[string]any{"error": map[string]string{"code": code, "message": msg}})
	_, _ = w.Write(b)
}
