// Package httpx holds the HTTP plumbing shared by every route: request IDs,
// JSON access logs, panic recovery, the 64 KB body cap, JSON decoding helpers,
// CSRF protection (CrossOriginProtection + Origin + X-Requested-With) and
// /healthz.
//
// Owner: workstream I1. Wrap and Register are called from main.go; their
// signatures are frozen, their bodies are I1's to grow. The current Wrap is a
// minimal placeholder (request ID + access log).
package httpx
