package apperr

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/contractsdir"
)

func decode(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var got map[string]any
	dec := json.NewDecoder(rec.Body)
	if err := dec.Decode(&got); err != nil {
		t.Fatalf("decode body: %v", err)
	}
	return got
}

func TestWriteErrorShape(t *testing.T) {
	rec := httptest.NewRecorder()
	WriteError(rec, http.StatusBadRequest, CodeInvalidJSON, "body is not valid JSON")
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json; charset=utf-8" {
		t.Fatalf("Content-Type = %q", ct)
	}
	if rec.Header().Get("X-Content-Type-Options") != "nosniff" {
		t.Fatal("missing nosniff")
	}
	got := decode(t, rec)
	want := map[string]any{"error": map[string]any{"code": "invalid_json", "message": "body is not valid JSON"}}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("body = %v, want %v", got, want)
	}
}

func TestWriteAppError(t *testing.T) {
	rec := httptest.NewRecorder()
	err := fmt.Errorf("wrapped: %w", New(http.StatusNotFound, CodeNotFound, "no such movie"))
	Write(rec, err)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d", rec.Code)
	}
	e := decode(t, rec)["error"].(map[string]any)
	if e["code"] != "not_found" || e["message"] != "no such movie" {
		t.Fatalf("error = %v", e)
	}
}

func TestWriteOpaqueErrorDoesNotLeak(t *testing.T) {
	rec := httptest.NewRecorder()
	Write(rec, errors.New("sql: secret table users_pw at /var/data/app.db"))
	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d", rec.Code)
	}
	body := rec.Body.String()
	if strings.Contains(body, "secret") || strings.Contains(body, "app.db") {
		t.Fatalf("internal details leaked: %s", body)
	}
	if !strings.Contains(body, `"code":"internal"`) {
		t.Fatalf("body = %s", body)
	}
}

func TestWriteJSONUnencodable(t *testing.T) {
	rec := httptest.NewRecorder()
	WriteJSON(rec, http.StatusOK, map[string]float64{"x": math.NaN()})
	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d", rec.Code)
	}
	if e := decode(t, rec)["error"].(map[string]any); e["code"] != "internal" {
		t.Fatalf("error = %v", e)
	}
}

func TestErrorString(t *testing.T) {
	e := New(http.StatusTooManyRequests, CodeRateLimited, "slow down")
	if e.Error() != "429 rate_limited: slow down" {
		t.Fatalf("Error() = %q", e.Error())
	}
}

// TestCodesMatchContract checks AllCodes against the ErrorCode enum in
// contracts/public.yaml, in the same order.
func TestCodesMatchContract(t *testing.T) {
	dir, err := contractsdir.Find()
	if err != nil {
		t.Fatalf("locate contracts: %v", err)
	}
	raw, err := os.ReadFile(filepath.Join(dir, "public.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?s)\n    ErrorCode:\s*\n\s+type: string\s*\n\s+enum: \[([^\]]*)\]`).FindSubmatch(raw)
	if m == nil {
		t.Fatal("ErrorCode enum not found in public.yaml")
	}
	var contract []string
	for _, f := range strings.Split(string(m[1]), ",") {
		if f = strings.TrimSpace(f); f != "" {
			contract = append(contract, f)
		}
	}
	var ours []string
	for _, c := range AllCodes {
		ours = append(ours, string(c))
	}
	if strings.Join(contract, ",") != strings.Join(ours, ",") {
		t.Fatalf("ErrorCode enum mismatch:\ncontract: %v\napperr:   %v", contract, ours)
	}
}
