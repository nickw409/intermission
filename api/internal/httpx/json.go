package httpx

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// Public errors returned by DecodeJSON. Messages match contracts/fixtures/public.
var (
	ErrBodyTooLarge = apperr.New(http.StatusRequestEntityTooLarge, apperr.CodeBodyTooLarge, "request body exceeds 64 KB")
	ErrInvalidJSON  = apperr.New(http.StatusBadRequest, apperr.CodeInvalidJSON, "request body is not valid JSON")
	ErrUnknownField = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "request body has an unknown field")
	ErrWrongType    = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "request body has a field of the wrong type")
)

// DecodeJSON reads the request body (at most MaxBodyBytes) and decodes exactly
// one JSON value into v, rejecting unknown fields. Every error it returns is
// an *apperr.Error ready for apperr.Write:
//
//   - body over 64 KB                       → 413 body_too_large
//   - not JSON, empty, or trailing data     → 400 invalid_json
//   - unknown field or wrong type           → 400 bad_request
//
// Field-level rules (lengths, patterns) are the caller's job.
func DecodeJSON(r *http.Request, v any) error {
	data, err := ReadBody(r)
	if err != nil {
		return err
	}
	return DecodeJSONBytes(data, v)
}

// ReadBody reads the whole request body, at most MaxBodyBytes. A larger body
// is ErrBodyTooLarge; a failed read is ErrInvalidJSON.
func ReadBody(r *http.Request) ([]byte, error) {
	if r.ContentLength > MaxBodyBytes {
		return nil, ErrBodyTooLarge
	}
	if r.Body == nil {
		return nil, nil
	}
	data, err := io.ReadAll(http.MaxBytesReader(nil, r.Body, MaxBodyBytes))
	if err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			return nil, ErrBodyTooLarge
		}
		return nil, ErrInvalidJSON
	}
	return data, nil
}

// DecodeJSONBytes decodes exactly one JSON value from data into v, rejecting
// unknown fields; errors as for DecodeJSON. Fuzz targets call it directly.
func DecodeJSONBytes(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		var typeErr *json.UnmarshalTypeError
		switch {
		case errors.As(err, &typeErr):
			return ErrWrongType
		case strings.HasPrefix(err.Error(), "json: unknown field "):
			return ErrUnknownField
		default:
			return ErrInvalidJSON
		}
	}
	// Exactly one value: anything after it but whitespace is invalid.
	if _, err := dec.Token(); err != io.EOF {
		return ErrInvalidJSON
	}
	return nil
}
