// Package integrationtest runs Intermission's real services together (gate N1:
// contract, both sides). See README.md.
//
// TestMain trains a tiny artifact with the real recs-train, starts the real
// recs-model on a Unix socket and the real recs-api (built from api/) in
// front of it, runs the tests, then checks every recorded HTTP exchange
// against contracts/*.yaml with validate-observations.mjs.
//
// Opt-in: the tests run only with RECS_INTEGRATION=1 (run-contract.sh sets
// it). RECS_MODEL_BIN and RECS_TRAIN_BIN default to build/release/.
package integrationtest

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// env is the running stack shared by every test.
var env *stack

type stack struct {
	repo      string
	work      string // scratch directory, removed at exit
	sockDir   string // short directory for Unix sockets (108-byte path limit)
	modelBin  string
	trainBin  string
	apiBin    string
	artifacts string
	version   string // artifact version CURRENT points at

	modelSock string
	model     *exec.Cmd

	apiAddr   string
	apiEnv    []string
	origin    string
	api       *exec.Cmd
	apiStderr *lockedBuffer

	obs *observer
}

func TestMain(m *testing.M) {
	if os.Getenv("RECS_INTEGRATION") != "1" {
		fmt.Println("integration tests skipped: set RECS_INTEGRATION=1 (tests/integration/run-contract.sh does)")
		os.Exit(0)
	}
	s, err := start()
	if err != nil {
		fmt.Fprintln(os.Stderr, "integration setup failed:", err)
		if s != nil {
			s.stop()
		}
		os.Exit(1)
	}
	env = s
	code := m.Run()
	s.stop()
	if code == 0 {
		code = s.validate()
	}
	if os.Getenv("RECS_INTEGRATION_KEEP") == "" {
		_ = os.RemoveAll(s.work)
	}
	_ = os.RemoveAll(s.sockDir)
	os.Exit(code)
}

func repoRoot() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for dir := wd; ; dir = filepath.Dir(dir) {
		if _, err := os.Stat(filepath.Join(dir, "contracts", "model.yaml")); err == nil {
			return dir, nil
		}
		if filepath.Dir(dir) == dir {
			return "", errors.New("repository root (contracts/model.yaml) not found")
		}
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func start() (*stack, error) {
	repo, err := repoRoot()
	if err != nil {
		return nil, err
	}
	s := &stack{repo: repo}
	s.modelBin = envOr("RECS_MODEL_BIN", filepath.Join(repo, "build/release/model-service/recs-model"))
	s.trainBin = envOr("RECS_TRAIN_BIN", filepath.Join(repo, "build/release/train/recs-train"))
	for _, b := range []string{s.modelBin, s.trainBin} {
		if _, err := os.Stat(b); err != nil {
			return nil, fmt.Errorf("%s missing (build with: cmake --preset release && cmake --build --preset release --target recs-model recs-train): %w", b, err)
		}
	}
	if s.work, err = os.MkdirTemp("", "intermission-it-"); err != nil {
		return nil, err
	}
	// Unix socket paths are limited to 107 bytes; TMPDIR may be long.
	if s.sockDir, err = os.MkdirTemp(shortTempRoot(), "it"); err != nil {
		return s, err
	}
	s.obs = &observer{path: filepath.Join(s.work, "observations.jsonl")}

	if err := s.trainTiny(); err != nil {
		return s, fmt.Errorf("train tiny artifact: %w", err)
	}
	s.modelSock = filepath.Join(s.sockDir, "model.sock")
	if s.model, err = startModel(s.modelBin, s.modelSock, s.artifacts, filepath.Join(s.work, "model.log")); err != nil {
		return s, err
	}
	if err := waitFor(10*time.Second, func() bool { return socketGet(s.modelSock, "/healthz") == 200 }); err != nil {
		return s, fmt.Errorf("recs-model never became healthy (see %s): %w", filepath.Join(s.work, "model.log"), err)
	}
	if err := s.buildAPI(); err != nil {
		return s, err
	}
	if err := s.startAPI(); err != nil {
		return s, err
	}
	return s, nil
}

func shortTempRoot() string {
	if d := os.TempDir(); len(d) < 40 {
		return d
	}
	return "/tmp"
}

// trainTiny trains every model on tests/fixtures/tiny (20 users, 30 movies)
// with configs small enough for that catalog, and points CURRENT at it.
func (s *stack) trainTiny() error {
	raw := filepath.Join(s.work, "data", "raw", "tiny")
	if err := os.MkdirAll(raw, 0o755); err != nil {
		return err
	}
	for _, f := range []string{"movies.csv", "ratings.csv", "links.csv", "tags.csv"} {
		b, err := os.ReadFile(filepath.Join(s.repo, "tests", "fixtures", "tiny", f))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(raw, f), b, 0o644); err != nil {
			return err
		}
	}
	configs := filepath.Join(s.work, "configs")
	if err := os.MkdirAll(configs, 0o755); err != nil {
		return err
	}
	tiny := map[string]string{
		"pop":       `{ "m": 2 }`,
		"pop_count": `{}`,
		"knn":       `{ "neighbors": 20, "shrinkage": 10, "min_support": 2 }`,
		"content":   `{ "min_tag_count": 1, "genre_weight": 1.0 }`,
		"mf":        `{ "k": 4, "lambda": 0.05, "lr": 0.01, "lr_decay": 0.95, "max_epochs": 10, "patience": 3, "init_sd": 0.1 }`,
		"als":       `{ "k": 4, "lambda": 0.05, "alpha": 40, "iters": 5, "interaction_threshold": 3.5, "init_sd": 0.01 }`,
	}
	for name, cfg := range tiny {
		if err := os.WriteFile(filepath.Join(configs, name+".json"), []byte(cfg), 0o644); err != nil {
			return err
		}
	}
	s.artifacts = filepath.Join(s.work, "artifacts")
	cmd := exec.Command(s.trainBin, "train", "--dataset", "tiny", "--data-dir", filepath.Join(s.work, "data"),
		"--out", s.artifacts, "--config-dir", configs, "--models", "pop,pop_count,knn,content,mf,als",
		"--default-model", "knn", "--set-current", "--seed", "42")
	cmd.Env = append(os.Environ(), "OMP_NUM_THREADS=2")
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("%v: %s", err, out)
	}
	cur, err := os.ReadFile(filepath.Join(s.artifacts, "CURRENT"))
	if err != nil {
		return err
	}
	s.version = strings.TrimSpace(string(cur))
	return nil
}

func startModel(bin, sock, artifacts, logPath string) (*exec.Cmd, error) {
	logf, err := os.Create(logPath)
	if err != nil {
		return nil, err
	}
	cmd := exec.Command(bin, "--socket", sock, "--artifacts", artifacts, "--poll-seconds", "1", "--threads", "4")
	cmd.Env = append(os.Environ(), "OMP_NUM_THREADS=2")
	cmd.Stdout, cmd.Stderr = logf, logf
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return cmd, nil
}

func (s *stack) buildAPI() error {
	s.apiBin = filepath.Join(s.work, "recs-api")
	cmd := exec.Command("go", "build", "-o", s.apiBin, "./cmd/recs-api")
	cmd.Dir = filepath.Join(s.repo, "api")
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("go build recs-api: %v: %s", err, out)
	}
	return nil
}

func freePort() (string, error) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", err
	}
	defer l.Close()
	return l.Addr().String(), nil
}

func (s *stack) startAPI() error {
	addr, err := freePort()
	if err != nil {
		return err
	}
	s.apiAddr = addr
	s.origin = "http://" + addr
	reports := filepath.Join(s.work, "reports")
	if err := os.MkdirAll(reports, 0o755); err != nil {
		return err
	}
	// The contract fixture reports (one per model, plus an unknown schema major).
	src := filepath.Join(s.repo, "contracts", "fixtures", "reports")
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".json") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(src, e.Name()))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(reports, e.Name()), b, 0o644); err != nil {
			return err
		}
	}
	s.apiEnv = append(os.Environ(),
		"RECS_ADDR="+addr,
		"RECS_DB_PATH="+filepath.Join(s.work, "app.db"),
		"RECS_MODEL_SOCKET="+s.modelSock,
		"RECS_MODEL_ADDR=",
		"RECS_SITE_ORIGIN="+s.origin,
		"RECS_SIGNUP_MODE=invite",
		"RECS_REPORTS_DIR="+reports,
		"RECS_EXPORTS_DIR="+filepath.Join(s.work, "exports"),
		"RECS_POSTER_CACHE_DIR="+filepath.Join(s.work, "posters"),
		"RECS_LOG_LEVEL=info",
		"TMDB_API_KEY=",
	)
	s.apiStderr = &lockedBuffer{}
	cmd := exec.Command(s.apiBin, "serve")
	cmd.Env = s.apiEnv
	cmd.Stdout, cmd.Stderr = s.apiStderr, s.apiStderr
	if err := cmd.Start(); err != nil {
		return err
	}
	s.api = cmd
	if err := waitFor(15*time.Second, func() bool {
		resp, err := http.Get("http://" + addr + "/healthz")
		if err != nil {
			return false
		}
		resp.Body.Close()
		return resp.StatusCode == 200
	}); err != nil {
		return fmt.Errorf("recs-api never became healthy: %w\n%s", err, s.apiStderr.String())
	}
	return nil
}

// invite mints an invite code with the real CLI against the same database.
func (s *stack) invite(uses int) (string, error) {
	cmd := exec.Command(s.apiBin, "admin", "invite", "create", "--uses", fmt.Sprint(uses), "--expires", "1d", "--note", "integration")
	cmd.Env = s.apiEnv
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("invite create: %v: %s", err, errb.String())
	}
	return strings.TrimSpace(out.String()), nil
}

func stopCmd(c *exec.Cmd) {
	if c == nil || c.Process == nil || c.ProcessState != nil {
		return
	}
	_ = c.Process.Signal(os.Interrupt)
	done := make(chan struct{})
	go func() { _ = c.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		_ = c.Process.Kill()
		<-done
	}
}

func (s *stack) stop() {
	stopCmd(s.api)
	stopCmd(s.model)
}

// stopModel stops recs-model (degradation tests).
func (s *stack) stopModel() { stopCmd(s.model) }

func (s *stack) validate() int {
	if err := s.obs.flush(); err != nil {
		fmt.Fprintln(os.Stderr, "writing observations:", err)
		return 1
	}
	cmd := exec.Command("node", filepath.Join(s.repo, "tests", "integration", "validate-observations.mjs"), s.obs.path, "--require-coverage")
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "contract validation failed:", err)
		if out := os.Getenv("RECS_INTEGRATION_OBS_OUT"); out != "" {
			_ = copyFile(s.obs.path, out)
		}
		return 1
	}
	if out := os.Getenv("RECS_INTEGRATION_OBS_OUT"); out != "" {
		_ = copyFile(s.obs.path, out)
	}
	return 0
}

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, b, 0o644)
}

func waitFor(d time.Duration, ok func() bool) error {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if ok() {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("timed out after %s", d)
}

// ---- HTTP over the model socket ----

func socketClient(sock string) *http.Client {
	return &http.Client{
		Timeout: 5 * time.Second,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				var d net.Dialer
				return d.DialContext(ctx, "unix", sock)
			},
			DisableCompression: true,
		},
	}
}

func socketGet(sock, path string) int {
	resp, err := socketClient(sock).Get("http://recs-model" + path)
	if err != nil {
		return 0
	}
	resp.Body.Close()
	return resp.StatusCode
}

// ---- recorded exchanges ----

// observation is one HTTP exchange, checked later against the contract.
type observation struct {
	Spec        string            `json:"spec"`
	OperationID string            `json:"operationId"`
	Case        string            `json:"case"`
	Method      string            `json:"method"`
	Path        string            `json:"path"`
	Status      int               `json:"status"`
	Headers     map[string]string `json:"headers"`
	Body        json.RawMessage   `json:"body"`
	ContentType string            `json:"contentType"`
}

type observer struct {
	path string
	mu   sync.Mutex
	list []observation
}

func (o *observer) add(ob observation) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.list = append(o.list, ob)
}

func (o *observer) flush() error {
	o.mu.Lock()
	defer o.mu.Unlock()
	f, err := os.Create(o.path)
	if err != nil {
		return err
	}
	w := bufio.NewWriter(f)
	enc := json.NewEncoder(w)
	for _, ob := range o.list {
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

// response is what a test sees of one exchange.
type response struct {
	Status  int
	Header  http.Header
	Body    []byte
	Elapsed time.Duration
}

func (r response) json(t *testing.T, v any) {
	t.Helper()
	if err := json.Unmarshal(r.Body, v); err != nil {
		t.Fatalf("decode %q: %v", truncate(r.Body), err)
	}
}

func (r response) errorCode() string {
	var e struct {
		Error struct {
			Code string `json:"code"`
		} `json:"error"`
	}
	_ = json.Unmarshal(r.Body, &e)
	return e.Error.Code
}

func truncate(b []byte) string {
	if len(b) > 300 {
		return string(b[:300]) + "..."
	}
	return string(b)
}

// request describes one exchange to send and record.
type request struct {
	spec, op, name string
	method, path   string
	headers        map[string]string
	body           []byte
}

// do sends req with client to base and records the exchange.
func do(t *testing.T, client *http.Client, base string, req request) response {
	t.Helper()
	var body io.Reader
	if req.body != nil {
		body = bytes.NewReader(req.body)
	}
	hr, err := http.NewRequest(req.method, base+req.path, body)
	if err != nil {
		t.Fatalf("%s: %v", req.name, err)
	}
	for k, v := range req.headers {
		hr.Header.Set(k, v)
	}
	if req.body != nil && hr.Header.Get("Content-Type") == "" {
		hr.Header.Set("Content-Type", "application/json")
	}
	start := time.Now()
	resp, err := client.Do(hr)
	if err != nil {
		t.Fatalf("%s %s %s: %v", req.name, req.method, req.path, err)
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("%s: read body: %v", req.name, err)
	}
	hdrs := map[string]string{}
	for k := range resp.Header {
		hdrs[strings.ToLower(k)] = resp.Header.Get(k)
	}
	ob := observation{
		Spec: req.spec, OperationID: req.op, Case: req.name, Method: req.method, Path: req.path,
		Status: resp.StatusCode, Headers: hdrs, ContentType: resp.Header.Get("Content-Type"),
	}
	if strings.HasPrefix(ob.ContentType, "application/json") && json.Valid(data) {
		ob.Body = data
	}
	env.obs.add(ob)
	return response{Status: resp.StatusCode, Header: resp.Header, Body: data, Elapsed: elapsed}
}

// lockedBuffer collects a child's output safely.
type lockedBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *lockedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}
