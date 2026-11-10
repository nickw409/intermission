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
	"bytes"
	"context"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/integrationtest/internal/record"
	"github.com/nickw409/intermission/api/internal/db"
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

	obs *record.Recorder
}

func TestMain(m *testing.M) {
	if os.Getenv("RECS_INTEGRATION") != "1" {
		// Every test calls needStack and reports SKIP (visible with -v), so a run without the
		// opt-in never looks like a pass of the real gate.
		os.Exit(m.Run())
	}
	s, err := start()
	if err != nil {
		fmt.Fprintln(os.Stderr, "FAIL: integration setup:", err)
		if s != nil {
			s.stop()
			if b, err := os.ReadFile(filepath.Join(s.work, "model.log")); err == nil && len(b) > 0 {
				fmt.Fprintf(os.Stderr, "recs-model log (end):\n%s\n", b[max(0, len(b)-2000):])
			}
			s.cleanup(true)
		}
		os.Exit(1)
	}
	env = s
	code := m.Run()
	s.stop()
	if code == 0 {
		code = s.validate()
	}
	s.cleanup(code == 0 && os.Getenv("RECS_INTEGRATION_KEEP") == "")
	os.Exit(code)
}

// needStack skips the test unless the opt-in stack is running.
func needStack(t *testing.T) {
	t.Helper()
	if env == nil {
		t.Skip("integration gate not run: set RECS_INTEGRATION=1 (tests/integration/run-contract.sh does)")
	}
}

// cleanup removes the socket directory, and the scratch directory unless it should be kept
// for debugging (on failure), in which case the logs' paths are printed.
func (s *stack) cleanup(removeWork bool) {
	if s.sockDir != "" {
		_ = os.RemoveAll(s.sockDir)
	}
	if s.work == "" {
		return
	}
	if removeWork {
		_ = os.RemoveAll(s.work)
		return
	}
	fmt.Fprintf(os.Stderr, "kept for debugging: %s (recs-model log: %s; remove it when done)\n",
		s.work, filepath.Join(s.work, "model.log"))
	if s.apiStderr != nil {
		_ = os.WriteFile(filepath.Join(s.work, "api.log"), []byte(s.apiStderr.String()), 0o600)
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func start() (*stack, error) {
	repo, err := record.RepoRoot()
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
	s.obs = &record.Recorder{}

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

// seedRatings tops the user's ratings up to total by writing rows for movie IDs outside the
// catalog (1,000,001 and up) straight into the harness's own scratch app.db, in one
// transaction, the way recs-api writes them (bumping ratings_version too).
func (s *stack) seedRatings(username string, total int) error {
	ctx := context.Background()
	d, err := db.Open(ctx, filepath.Join(s.work, "app.db"))
	if err != nil {
		return err
	}
	defer d.Close()
	tx, err := d.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	var uid int64
	var have int
	if err := tx.QueryRowContext(ctx, "SELECT id FROM users WHERE username = ?", username).Scan(&uid); err != nil {
		return fmt.Errorf("user %s: %w", username, err)
	}
	if err := tx.QueryRowContext(ctx, "SELECT count(*) FROM ratings WHERE user_id = ?", uid).Scan(&have); err != nil {
		return err
	}
	now := time.Now().Unix()
	for i := 0; i < total-have; i++ {
		if _, err := tx.ExecContext(ctx, "INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, 3.0, ?)",
			uid, 1_000_001+i, now); err != nil {
			return err
		}
	}
	if _, err := tx.ExecContext(ctx, "UPDATE users SET ratings_version = ratings_version + 1 WHERE id = ?", uid); err != nil {
		return err
	}
	return tx.Commit()
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

// validate checks every recorded exchange against the contracts, requiring
// every documented status of every operation to have been seen.
func (s *stack) validate() int {
	return s.obs.Validate(s.repo, filepath.Join(s.work, "observations.jsonl"), true)
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

// ---- recorded exchanges (package record) ----

type response = record.Response

// request is one exchange against the native stack.
type request struct {
	spec, op, name string
	method, path   string
	headers        map[string]string
	body           []byte
}

// do sends req and records it for the contract check.
func do(t *testing.T, client *http.Client, base string, req request) response {
	t.Helper()
	return env.obs.Do(t, client, base, record.Request{Spec: req.spec, Op: req.op, Name: req.name,
		Method: req.method, Path: req.path, Headers: req.headers, Body: req.body})
}

func truncate(b []byte) string { return record.Truncate(b) }
