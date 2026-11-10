// Package apperr defines the public API error shape and JSON response helpers.
//
// Owner: spine (orchestrator). Every error leaving recs-api uses the shape
// {"error":{"code":"...","message":"..."}} from contracts/public.yaml, with a
// Code from the ErrorCode enum.
package apperr

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
)

// Code is a machine-readable error code from the ErrorCode enum in
// contracts/public.yaml.
type Code string

// Every value of the ErrorCode enum in contracts/public.yaml. A test checks
// that this list and the contract stay in sync.
const (
	CodeBadRequest         Code = "bad_request"
	CodeInvalidJSON        Code = "invalid_json"
	CodeBodyTooLarge       Code = "body_too_large"
	CodeUnauthorized       Code = "unauthorized"
	CodeInvalidCredentials Code = "invalid_credentials"
	CodeCSRFRejected       Code = "csrf_rejected"
	CodeInvalidInvite      Code = "invalid_invite"
	CodeUsernameTaken      Code = "username_taken"
	CodeTooManyRatings     Code = "too_many_ratings"
	CodeNotFound           Code = "not_found"
	CodeUnknownModel       Code = "unknown_model"
	CodeRateLimited        Code = "rate_limited"
	CodeModelUnavailable   Code = "model_unavailable"
	CodeInternal           Code = "internal"
)

// AllCodes lists every Code in contract order.
var AllCodes = []Code{
	CodeBadRequest, CodeInvalidJSON, CodeBodyTooLarge, CodeUnauthorized,
	CodeInvalidCredentials, CodeCSRFRejected, CodeInvalidInvite,
	CodeUsernameTaken, CodeTooManyRatings, CodeNotFound, CodeUnknownModel,
	CodeRateLimited, CodeModelUnavailable, CodeInternal,
}

// Error is an error that maps to a specific HTTP status and public error body.
// Message is shown to the client, so it must never contain secrets, SQL,
// file paths or rating payloads.
type Error struct {
	Status  int
	Code    Code
	Message string
}

func (e *Error) Error() string {
	return fmt.Sprintf("%d %s: %s", e.Status, e.Code, e.Message)
}

// New returns an *Error.
func New(status int, code Code, message string) *Error {
	return &Error{Status: status, Code: code, Message: message}
}

// Body is the JSON shape of an error response.
type Body struct {
	Error BodyError `json:"error"`
}

// BodyError is the inner object of Body.
type BodyError struct {
	Code    Code   `json:"code"`
	Message string `json:"message"`
}

// WriteJSON writes v as a JSON response with the given status.
func WriteJSON(w http.ResponseWriter, status int, v any) {
	buf, err := json.Marshal(v)
	if err != nil {
		// A marshal failure is a programming error; never leak details.
		slog.Error("apperr: encoding response", "err", err)
		writeBody(w, http.StatusInternalServerError, []byte(`{"error":{"code":"internal","message":"internal error"}}`))
		return
	}
	writeBody(w, status, buf)
}

// WriteError writes the standard error body with the given status.
func WriteError(w http.ResponseWriter, status int, code Code, message string) {
	WriteJSON(w, status, Body{Error: BodyError{Code: code, Message: message}})
}

// Write writes err as an error response. An *Error (possibly wrapped) is
// written as-is; anything else becomes a 500 "internal" response whose
// message reveals nothing about err. Callers log err themselves if needed.
func Write(w http.ResponseWriter, err error) {
	var e *Error
	if errors.As(err, &e) {
		WriteError(w, e.Status, e.Code, e.Message)
		return
	}
	WriteError(w, http.StatusInternalServerError, CodeInternal, "internal error")
}

func writeBody(w http.ResponseWriter, status int, body []byte) {
	h := w.Header()
	h.Set("Content-Type", "application/json; charset=utf-8")
	h.Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}
