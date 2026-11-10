// Package record sends HTTP requests for the integration tests and records
// every exchange, so validate-observations.mjs can check them all against
// contracts/*.yaml after the run.
package record

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// Observation is one HTTP exchange (the line format validate-observations.mjs reads).
type Observation struct {
	Spec        string            `json:"spec"`
	OperationID string            `json:"operationId"`
	Case        string            `json:"case"`
	Method      string            `json:"method"`
	Path        string            `json:"path"`
	Status      int               `json:"status"`
	Headers     map[string]string `json:"headers"`
	Body        json.RawMessage   `json:"body"`
	ContentType string            `json:"contentType"`
	BodyBytes   int               `json:"bodyBytes"`
}

// Recorder collects observations; safe for concurrent use.
type Recorder struct {
	mu   sync.Mutex
	list []Observation
}

// Request is one exchange to send and record.
type Request struct {
	Spec, Op, Name string // contract ("model" or "public"), operationId, case name
	Method, Path   string
	Headers        map[string]string // "Host" sets the request's Host
	Body           []byte            // sent as application/json unless Content-Type is set
}

// Response is what a test sees of one exchange.
type Response struct {
	Status  int
	Header  http.Header
	Body    []byte
	Elapsed time.Duration // until the whole body was read
}

// JSON decodes the body into v or fails the test.
func (r Response) JSON(t *testing.T, v any) {
	t.Helper()
	if err := json.Unmarshal(r.Body, v); err != nil {
		t.Fatalf("decode %q: %v", Truncate(r.Body), err)
	}
}

// ErrorCode is error.code of an Error body, or "".
func (r Response) ErrorCode() string {
	var e struct {
		Error struct {
			Code string `json:"code"`
		} `json:"error"`
	}
	_ = json.Unmarshal(r.Body, &e)
	return e.Error.Code
}

// Truncate shortens a body for messages.
func Truncate(b []byte) string {
	if len(b) > 300 {
		return string(b[:300]) + "..."
	}
	return string(b)
}

// Do sends req with client to base and records the exchange.
func (rec *Recorder) Do(t *testing.T, client *http.Client, base string, req Request) Response {
	t.Helper()
	var body io.Reader
	if req.Body != nil {
		body = bytes.NewReader(req.Body)
	}
	hr, err := http.NewRequest(req.Method, base+req.Path, body)
	if err != nil {
		t.Fatalf("%s: %v", req.Name, err)
	}
	for k, v := range req.Headers {
		if strings.EqualFold(k, "Host") {
			hr.Host = v
			continue
		}
		hr.Header.Set(k, v)
	}
	if req.Body != nil && hr.Header.Get("Content-Type") == "" {
		hr.Header.Set("Content-Type", "application/json")
	}
	start := time.Now()
	resp, err := client.Do(hr)
	if err != nil {
		t.Fatalf("%s %s %s: %v", req.Name, req.Method, req.Path, err)
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("%s: read body: %v", req.Name, err)
	}
	hdrs := map[string]string{}
	for k := range resp.Header {
		hdrs[strings.ToLower(k)] = resp.Header.Get(k)
	}
	// Session tokens never reach the observations file: only the fact that a cookie was set
	// (contract checks need its presence on signup and login).
	for _, k := range []string{"set-cookie", "cookie"} {
		if _, ok := hdrs[k]; ok {
			hdrs[k] = "[redacted]"
		}
	}
	ob := Observation{
		Spec: req.Spec, OperationID: req.Op, Case: req.Name, Method: req.Method, Path: req.Path,
		Status: resp.StatusCode, Headers: hdrs, ContentType: resp.Header.Get("Content-Type"), BodyBytes: len(data),
	}
	if strings.HasPrefix(ob.ContentType, "application/json") && json.Valid(data) {
		ob.Body = data
	}
	rec.mu.Lock()
	rec.list = append(rec.list, ob)
	rec.mu.Unlock()
	return Response{Status: resp.StatusCode, Header: resp.Header, Body: data, Elapsed: elapsed}
}

// Write saves the observations as JSON lines.
func (rec *Recorder) Write(path string) error {
	rec.mu.Lock()
	defer rec.mu.Unlock()
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	w := bufio.NewWriter(f)
	enc := json.NewEncoder(w)
	for _, ob := range rec.list {
		if err := enc.Encode(ob); err != nil {
			f.Close()
			return err
		}
	}
	if err := w.Flush(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// Validate writes the observations to path and runs validate-observations.mjs
// on them (from repo). It returns a process exit code. If RECS_INTEGRATION_OBS_OUT
// is set, the observations are also copied there.
func (rec *Recorder) Validate(repo, path string, requireCoverage bool) int {
	if err := rec.Write(path); err != nil {
		fmt.Fprintln(os.Stderr, "writing observations:", err)
		return 1
	}
	if out := os.Getenv("RECS_INTEGRATION_OBS_OUT"); out != "" {
		if b, err := os.ReadFile(path); err == nil {
			_ = os.WriteFile(out, b, 0o644)
		}
	}
	args := []string{filepath.Join(repo, "tests", "integration", "validate-observations.mjs"), path}
	if requireCoverage {
		args = append(args, "--require-coverage")
	}
	cmd := exec.Command("node", args...)
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "contract validation failed:", err)
		return 1
	}
	return 0
}

// RepoRoot walks up from the working directory to the directory holding contracts/model.yaml.
func RepoRoot() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for dir := wd; ; dir = filepath.Dir(dir) {
		if _, err := os.Stat(filepath.Join(dir, "contracts", "model.yaml")); err == nil {
			return dir, nil
		}
		if filepath.Dir(dir) == dir {
			return "", fmt.Errorf("repository root (contracts/model.yaml) not found above %s", wd)
		}
	}
}
