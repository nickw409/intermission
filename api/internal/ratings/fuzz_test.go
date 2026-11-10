package ratings

import (
	"errors"
	"net/http"
	"testing"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// FuzzParsePut: any body either yields a valid rating or a 400 with the
// public error shape; nothing panics.
func FuzzParsePut(f *testing.F) {
	for _, s := range []string{`{"rating":4.5}`, `{"rating":0.5}`, `{"rating":4.3}`, `{"rating":"4"}`, `{}`, `{"rating":null}`,
		`{"rating":4.5,"rating":1}`, `{"Rating":4.5}`, `[1]`, `{not json`, `{"rating":1e999}`, "\x00", `{"rating":5}  `} {
		f.Add([]byte(s))
	}
	f.Fuzz(func(t *testing.T, body []byte) {
		r, err := parsePut(body)
		if err == nil {
			if !ValidRating(r) {
				t.Fatalf("accepted invalid rating %v from %q", r, body)
			}
			return
		}
		var ae *apperr.Error
		if !errors.As(err, &ae) || ae.Status != http.StatusBadRequest ||
			(ae.Code != apperr.CodeBadRequest && ae.Code != apperr.CodeInvalidJSON) {
			t.Fatalf("error %v for %q", err, body)
		}
	})
}

func TestValidRating(t *testing.T) {
	for r, want := range map[float64]bool{0.5: true, 1: true, 4.5: true, 5: true, 0: false, 0.25: false, 5.5: false, -0.5: false, 4.4999999: false} {
		if ValidRating(r) != want {
			t.Errorf("ValidRating(%v) != %v", r, want)
		}
	}
}
