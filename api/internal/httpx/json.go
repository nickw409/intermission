package httpx

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"reflect"
	"strings"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// Public errors returned by DecodeJSON. Messages match contracts/fixtures/public.
var (
	ErrBodyTooLarge   = apperr.New(http.StatusRequestEntityTooLarge, apperr.CodeBodyTooLarge, "request body exceeds 64 KB")
	ErrInvalidJSON    = apperr.New(http.StatusBadRequest, apperr.CodeInvalidJSON, "request body is not valid JSON")
	ErrUnknownField   = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "request body has an unknown field")
	ErrWrongType      = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "request body has a field of the wrong type")
	ErrDuplicateField = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "request body has a duplicate field")
)

// DecodeJSON reads the request body (at most MaxBodyBytes) and decodes exactly
// one JSON value into v, rejecting unknown fields. Every error it returns is
// an *apperr.Error ready for apperr.Write:
//
//   - body over 64 KB                       → 413 body_too_large
//   - not JSON, empty, or trailing data     → 400 invalid_json
//   - unknown, duplicate or wrong-case field, or wrong type → 400 bad_request
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
	return checkKeys(data, v)
}

// checkKeys closes two gaps in encoding/json for a top-level object:
// it silently keeps the last of duplicate keys, and it matches keys to
// struct fields case-insensitively. Both let two parsers (a proxy, a WAF,
// a log scrubber) read the same body differently, so: any key seen twice
// (after JSON unescaping, ignoring case) is ErrDuplicateField, and when v is
// a pointer to a struct every key must equal a field's JSON name exactly.
// Only top-level keys are checked; nested objects are not used by any
// request body. Embedded struct fields are not supported.
func checkKeys(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	if tok, err := dec.Token(); err != nil || tok != json.Delim('{') {
		return nil
	}
	known := jsonFieldNames(v)
	seen := map[string]bool{}
	for dec.More() {
		tok, err := dec.Token()
		if err != nil {
			return ErrInvalidJSON
		}
		key, _ := tok.(string)
		folded := strings.ToLower(key)
		if seen[folded] {
			return ErrDuplicateField
		}
		seen[folded] = true
		if known != nil && !known[key] {
			return ErrUnknownField
		}
		var skip json.RawMessage
		if err := dec.Decode(&skip); err != nil {
			return ErrInvalidJSON
		}
	}
	return nil
}

// jsonFieldNames returns the exact JSON key of every field of the struct v
// points to, or nil if v is not a pointer to a struct.
func jsonFieldNames(v any) map[string]bool {
	t := reflect.TypeOf(v)
	if t == nil || t.Kind() != reflect.Pointer || t.Elem().Kind() != reflect.Struct {
		return nil
	}
	t = t.Elem()
	names := map[string]bool{}
	for i := 0; i < t.NumField(); i++ {
		f := t.Field(i)
		if !f.IsExported() {
			continue
		}
		name, _, _ := strings.Cut(f.Tag.Get("json"), ",")
		switch name {
		case "-":
			continue
		case "":
			name = f.Name
		}
		names[name] = true
	}
	return names
}
