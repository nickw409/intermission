// Package reqid carries the per-request X-Request-ID through a context.Context.
//
// Owner: spine (orchestrator). It is a leaf package so that both the HTTP
// middleware (httpx) and the model client (modelclient) can use it without an
// import cycle.
package reqid

import (
	"context"
	"crypto/rand"
	"encoding/hex"
)

// Header is the HTTP header used to propagate the request ID across services.
const Header = "X-Request-ID"

// MaxLen is the longest caller-supplied request ID that is accepted as-is.
const MaxLen = 64

type ctxKey struct{}

// WithID returns a copy of ctx carrying id.
func WithID(ctx context.Context, id string) context.Context {
	return context.WithValue(ctx, ctxKey{}, id)
}

// FromContext returns the request ID stored in ctx, or "" if there is none.
func FromContext(ctx context.Context) string {
	id, _ := ctx.Value(ctxKey{}).(string)
	return id
}

// New returns a fresh random request ID (32 lowercase hex characters).
func New() string {
	var b [16]byte
	_, _ = rand.Read(b[:]) // crypto/rand.Read never returns an error on supported platforms.
	return hex.EncodeToString(b[:])
}

// Valid reports whether a caller-supplied request ID is safe to reuse and log:
// 1..MaxLen characters from [A-Za-z0-9._-].
func Valid(id string) bool {
	if len(id) == 0 || len(id) > MaxLen {
		return false
	}
	for i := 0; i < len(id); i++ {
		c := id[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '-', c == '_', c == '.':
		default:
			return false
		}
	}
	return true
}
