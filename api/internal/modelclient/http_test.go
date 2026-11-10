package modelclient_test

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// ---------------------------------------------------------------- helpers

// countingListener counts accepted connections and closes the first `fail`
// of them without answering, which the client sees as a connection error.
type countingListener struct {
	net.Listener
	fail     int32
	accepted atomic.Int32
}

func (l *countingListener) Accept() (net.Conn, error) {
	for {
		c, err := l.Listener.Accept()
		if err != nil {
			return nil, err
		}
		if n := l.accepted.Add(1); n <= l.fail {
			_ = c.Close()
			continue
		}
		return c, nil
	}
}

// server serves h on a loopback TCP listener that drops the first `fail`
// connections, and returns the listener and a client pointed at it.
func server(t *testing.T, h http.Handler, fail int32, timeout time.Duration) (*countingListener, modelclient.Client) {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	cl := &countingListener{Listener: ln, fail: fail}
	srv := &http.Server{Handler: h, ReadHeaderTimeout: 5 * time.Second}
	go func() { _ = srv.Serve(cl) }()
	t.Cleanup(func() { _ = srv.Close() })
	return cl, modelclient.NewHTTP(modelclient.HTTPConfig{Addr: ln.Addr().String(), Timeout: timeout})
}

func newFake(t *testing.T) *fake.Fake {
	t.Helper()
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func jsonEqual(a, b []byte) bool {
	var va, vb any
	if json.Unmarshal(a, &va) != nil || json.Unmarshal(b, &vb) != nil {
		return false
	}
	return reflect.DeepEqual(va, vb)
}

func decodeStrict(b []byte, v any) bool {
	b = bytes.TrimSpace(b)
	if len(b) == 0 || b[0] != '{' {
		return false
	}
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.DisallowUnknownFields()
	return dec.Decode(v) == nil
}

func queryOf(t *testing.T, c fake.Case) map[string]string {
	t.Helper()
	out := map[string]string{}
	for k, raw := range c.Request.Query {
		var s string
		if json.Unmarshal(raw, &s) != nil {
			s = strings.TrimSpace(string(raw))
		}
		out[k] = s
	}
	return out
}

// invoke calls the client method for opID with the arguments in the case's
// request. ok is false when the typed client can't express the request
// (raw invalid JSON bodies, /metrics).
func invoke(t *testing.T, c modelclient.Client, opID string, fc fake.Case) (resp any, meta modelclient.Meta, err error, ok bool) {
	t.Helper()
	ctx := context.Background()
	body := fc.Request.Body
	q := queryOf(t, fc)
	atoi := func(s string) int {
		if s == "" {
			return 0
		}
		n, err := strconv.Atoi(s)
		if err != nil {
			t.Fatalf("bad int %q", s)
		}
		return n
	}
	switch opID {
	case fake.OpRecommend:
		var r modelclient.RecommendRequest
		if !decodeStrict(body, &r) {
			return nil, meta, nil, false
		}
		resp, meta, err = c.Recommend(ctx, r)
	case fake.OpCompare:
		var r modelclient.CompareRequest
		if !decodeStrict(body, &r) {
			return nil, meta, nil, false
		}
		resp, meta, err = c.Compare(ctx, r)
	case fake.OpOnboarding:
		var r modelclient.OnboardingRequest
		if !decodeStrict(body, &r) {
			return nil, meta, nil, false
		}
		resp, meta, err = c.Onboarding(ctx, r)
	case fake.OpMoviesBatch:
		var r modelclient.BatchRequest
		if !decodeStrict(body, &r) {
			return nil, meta, nil, false
		}
		resp, meta, err = c.MoviesBatch(ctx, r.IDs)
	case fake.OpSimilar:
		id, perr := strconv.ParseInt(strings.TrimPrefix(fc.Request.Path, "/v1/similar/"), 10, 64)
		if perr != nil {
			return nil, meta, nil, false
		}
		resp, meta, err = c.Similar(ctx, id, modelclient.ModelName(q["model"]), atoi(q["k"]))
	case fake.OpSearch:
		resp, meta, err = c.Search(ctx, q["q"], atoi(q["limit"]))
	case fake.OpModels:
		resp, meta, err = c.Models(ctx)
	case fake.OpVersion:
		resp, meta, err = c.Version(ctx)
	case fake.OpHealthz:
		err = c.Healthz(ctx)
		resp = modelclient.Health{Status: "ok"}
	default:
		return nil, meta, nil, false
	}
	return resp, meta, err, true
}

func checkOutcome(t *testing.T, opID string, fc fake.Case, resp any, meta modelclient.Meta, err error) {
	t.Helper()
	if want := fc.Response.Header("X-Model-Version"); opID != fake.OpHealthz && meta.ModelVersion != want {
		t.Errorf("ModelVersion = %q, want %q", meta.ModelVersion, want)
	}
	switch {
	case fc.Response.Success():
		if err != nil {
			t.Fatalf("err = %v", err)
		}
		got, _ := json.Marshal(resp)
		if opID != fake.OpHealthz && !jsonEqual(got, fc.Response.Body) {
			t.Errorf("response\n got %s\nwant %s", got, fc.Response.Body)
		}
	case fc.Response.Status == http.StatusServiceUnavailable:
		if !errors.Is(err, modelclient.ErrUnavailable) {
			t.Errorf("err = %v, want ErrUnavailable", err)
		}
	default:
		var se *modelclient.StatusError
		var eb modelclient.ErrorBody
		_ = json.Unmarshal(fc.Response.Body, &eb)
		if !errors.As(err, &se) || se.Status != fc.Response.Status || se.Code != eb.Error.Code || se.Message != eb.Error.Message {
			t.Errorf("err = %v, want StatusError %d %s", err, fc.Response.Status, eb.Error.Code)
		}
		if errors.Is(err, modelclient.ErrUnavailable) {
			t.Errorf("a %d must not be ErrUnavailable", fc.Response.Status)
		}
	}
}

// ---------------------------------------------------------------- tests

func TestStatusErrorString(t *testing.T) {
	e := &modelclient.StatusError{Status: 400, Code: modelclient.ErrorCodeUnknownModel, Message: "no such model"}
	if got := e.Error(); got != "recs-model: 400 unknown_model: no such model" {
		t.Fatalf("Error() = %q", got)
	}
}

// Every case of every operation, over HTTP. Forced cases check response and
// error mapping; unforced success cases also check that the client sends
// exactly the request in the fixture (the fake answers with the case whose
// request matches, so the case name proves it).
func TestEveryFixtureCaseOverHTTP(t *testing.T) {
	f := newFake(t)
	_, c := server(t, fake.Handler(f), 0, time.Second)
	for opID, fx := range f.Fixtures() {
		for _, fc := range fx.Cases {
			t.Run(opID+"/"+fc.Name, func(t *testing.T) {
				if err := f.Force(opID, fc.Name); err != nil {
					t.Fatal(err)
				}
				resp, meta, err, ok := invoke(t, c, opID, fc)
				if err2 := f.Force(opID, ""); err2 != nil {
					t.Fatal(err2)
				}
				if !ok {
					t.Skip("request not expressible through the typed client")
				}
				checkOutcome(t, opID, fc, resp, meta, err)

				if !fc.Response.Success() {
					return
				}
				f.ResetCalls()
				resp, meta, err, _ = invoke(t, c, opID, fc)
				checkOutcome(t, opID, fc, resp, meta, err)
				calls := f.Calls()
				if len(calls) != 1 || calls[0].Case != fc.Name {
					t.Errorf("request did not match its own fixture case: calls=%+v", calls)
				}
			})
		}
	}
}

func TestUnixSocket(t *testing.T) {
	dir, err := os.MkdirTemp("", "mc") // short path: sun_path is ~108 bytes
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	sock := filepath.Join(dir, "model.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	f := newFake(t)
	srv := &http.Server{Handler: fake.Handler(f), ReadHeaderTimeout: time.Second}
	go func() { _ = srv.Serve(ln) }()
	t.Cleanup(func() { _ = srv.Close() })

	c := modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: sock})
	ctx := context.Background()
	m, meta, err := c.Models(ctx)
	if err != nil || m.DefaultModel != modelclient.ModelALS || meta.ModelVersion != "2026-11-09T21-14-03Z" {
		t.Fatalf("Models over unix socket: %+v %+v %v", m, meta, err)
	}
	b, _, err := c.MoviesBatch(ctx, []int64{4993, 999999999})
	if err != nil || len(b.Movies) != 1 || b.Movies[0].Title == "" || len(b.Missing) != 1 {
		t.Fatalf("MoviesBatch over unix socket: %+v %v", b, err)
	}
	if err := c.Healthz(ctx); err != nil {
		t.Fatalf("Healthz: %v", err)
	}
	f.SetDown(true)
	if err := c.Healthz(ctx); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("Healthz down: %v", err)
	}
	if _, _, err := c.Search(ctx, "star", 3); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("Search down: %v", err)
	}
}

func TestMissingSocketIsUnavailable(t *testing.T) {
	c := modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: filepath.Join(t.TempDir(), "nope.sock")})
	start := time.Now()
	if _, _, err := c.Models(context.Background()); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if _, _, err := c.MoviesBatch(context.Background(), []int64{1}); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if time.Since(start) > time.Second {
		t.Fatal("connection refused took too long")
	}
}

func TestRequestIDPropagated(t *testing.T) {
	f := newFake(t)
	_, c := server(t, fake.Handler(f), 0, time.Second)
	ctx := reqid.WithID(context.Background(), "rid-abc.123")
	if _, _, err := c.Version(ctx); err != nil {
		t.Fatal(err)
	}
	if _, _, err := c.MoviesBatch(ctx, []int64{260, 1196, 1210}); err != nil {
		t.Fatal(err)
	}
	// An invalid ID is never forwarded (the server mints its own).
	if _, _, err := c.Version(reqid.WithID(context.Background(), "bad id\r\nX-Evil: 1")); err != nil {
		t.Fatal(err)
	}
	calls := f.Calls()
	if len(calls) != 3 || calls[0].RequestID != "rid-abc.123" || calls[1].RequestID != "rid-abc.123" || calls[2].RequestID != "" {
		t.Fatalf("calls = %+v", calls)
	}
}

func TestOptionalQueryParams(t *testing.T) {
	f := newFake(t)
	_, c := server(t, fake.Handler(f), 0, time.Second)
	ctx := context.Background()
	payload := `star&limit=99#frag ' OR 1=1 --`
	_, _, _ = c.Search(ctx, payload, 0)
	_, _, _ = c.Similar(ctx, 260, "", 0)
	_, _, _ = c.Similar(ctx, 260, modelclient.ModelContent, 7)
	calls := f.Calls()
	if len(calls) != 3 {
		t.Fatalf("calls = %+v", calls)
	}
	if q := calls[0].Query; len(q) != 1 || q.Get("q") != payload {
		t.Errorf("search query = %v", q)
	}
	if q := calls[1].Query; len(q) != 0 || calls[1].Path != "/v1/similar/260" {
		t.Errorf("similar defaults: path %s query %v", calls[1].Path, q)
	}
	if q := calls[2].Query; q.Get("model") != "content" || q.Get("k") != "7" {
		t.Errorf("similar query = %v", q)
	}
}

// The deadline bounds the whole call, and a timeout is not retried.
func TestTimeout(t *testing.T) {
	f := newFake(t)
	f.SetLatency(3 * time.Second)
	ln, c := server(t, fake.Handler(f), 0, 100*time.Millisecond)
	start := time.Now()
	if _, _, err := c.Models(context.Background()); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if d := time.Since(start); d > time.Second {
		t.Fatalf("took %v", d)
	}
	if n := ln.accepted.Load(); n != 1 {
		t.Fatalf("timeout was retried: %d connections", n)
	}
}

func TestDefaultTimeoutIs500ms(t *testing.T) {
	f := newFake(t)
	f.SetLatency(3 * time.Second)
	_, c := server(t, fake.Handler(f), 0, 0)
	start := time.Now()
	if _, _, err := c.Search(context.Background(), "star", 3); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if d := time.Since(start); d < 450*time.Millisecond || d > time.Second {
		t.Fatalf("took %v, want about 500ms", d)
	}
}

// A caller's earlier deadline wins over the client's own.
func TestCallerDeadline(t *testing.T) {
	f := newFake(t)
	f.SetLatency(3 * time.Second)
	_, c := server(t, fake.Handler(f), 0, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	start := time.Now()
	if _, _, err := c.Version(ctx); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if d := time.Since(start); d > 300*time.Millisecond {
		t.Fatalf("took %v", d)
	}
}

func TestReadsRetryOnceOnConnectionError(t *testing.T) {
	f := newFake(t)
	ln, c := server(t, fake.Handler(f), 1, time.Second)
	m, _, err := c.Models(context.Background())
	if err != nil || m.ArtifactVersion == "" {
		t.Fatalf("Models after one dropped connection: %+v %v", m, err)
	}
	if n := ln.accepted.Load(); n != 2 {
		t.Fatalf("connections = %d, want 2", n)
	}
	if n := len(f.Calls()); n != 1 {
		t.Fatalf("server saw %d calls, want 1", n)
	}
}

func TestReadsRetryOnlyOnce(t *testing.T) {
	f := newFake(t)
	ln, c := server(t, fake.Handler(f), 2, time.Second)
	if _, _, err := c.Search(context.Background(), "star", 3); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if n := ln.accepted.Load(); n != 2 {
		t.Fatalf("connections = %d, want exactly 2 (one retry)", n)
	}
}

func TestPostsNeverRetry(t *testing.T) {
	for name, call := range map[string]func(modelclient.Client) error{
		"moviesBatch": func(c modelclient.Client) error {
			_, _, err := c.MoviesBatch(context.Background(), []int64{260, 1196, 1210})
			return err
		},
		"onboarding": func(c modelclient.Client) error {
			_, _, err := c.Onboarding(context.Background(), modelclient.OnboardingRequest{N: 4})
			return err
		},
		"recommend": func(c modelclient.Client) error {
			_, _, err := c.Recommend(context.Background(), modelclient.RecommendRequest{Model: "als", K: 3, Ratings: []modelclient.RatingIn{}})
			return err
		},
		"compare": func(c modelclient.Client) error {
			_, _, err := c.Compare(context.Background(), modelclient.CompareRequest{Models: []modelclient.ModelName{"pop"}, K: 3, Ratings: []modelclient.RatingIn{}})
			return err
		},
	} {
		t.Run(name, func(t *testing.T) {
			f := newFake(t)
			ln, c := server(t, fake.Handler(f), 1, time.Second)
			if err := call(c); !errors.Is(err, modelclient.ErrUnavailable) {
				t.Fatalf("err = %v", err)
			}
			if n := ln.accepted.Load(); n != 1 {
				t.Fatalf("POST was retried: %d connections", n)
			}
			if n := len(f.Calls()); n != 0 {
				t.Fatalf("server saw %d calls", n)
			}
		})
	}
}

// Responses are decoded strictly and size-capped; a malformed response is an
// error but not ErrUnavailable or a StatusError.
func TestStrictResponses(t *testing.T) {
	cases := map[string]func(w http.ResponseWriter){
		"unknown field": func(w http.ResponseWriter) {
			_, _ = w.Write([]byte(`{"artifact_version":"v","extra":1}`))
		},
		"trailing data": func(w http.ResponseWriter) {
			_, _ = w.Write([]byte(`{"artifact_version":"v"} {}`))
		},
		"wrong type": func(w http.ResponseWriter) {
			_, _ = w.Write([]byte(`{"artifact_version":7}`))
		},
		"not json": func(w http.ResponseWriter) {
			_, _ = w.Write([]byte(`<html>`))
		},
		"too large": func(w http.ResponseWriter) {
			_, _ = w.Write([]byte(`{"artifact_version":"`))
			_, _ = w.Write(bytes.Repeat([]byte("a"), modelclient.MaxResponseBytes))
			_, _ = w.Write([]byte(`"}`))
		},
	}
	for name, write := range cases {
		t.Run(name, func(t *testing.T) {
			_, c := server(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("X-Model-Version", "v")
				write(w)
			}), 0, 2*time.Second)
			_, _, err := c.Version(context.Background())
			var se *modelclient.StatusError
			if err == nil || errors.Is(err, modelclient.ErrUnavailable) || errors.As(err, &se) {
				t.Fatalf("err = %v", err)
			}
		})
	}
}

func TestErrorStatusMapping(t *testing.T) {
	status := 0
	body := ""
	_, c := server(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Model-Version", "v9")
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	}), 0, time.Second)
	ctx := context.Background()

	status, body = 503, `{"error":{"code":"model_unavailable","message":"x"}}`
	_, meta, err := c.Models(ctx)
	if !errors.Is(err, modelclient.ErrUnavailable) || meta.ModelVersion != "v9" {
		t.Fatalf("503: %v %+v", err, meta)
	}

	status, body = 404, `{"error":{"code":"not_found","message":"movie 5 not found"}}`
	var se *modelclient.StatusError
	if _, _, err := c.Similar(ctx, 5, "knn", 3); !errors.As(err, &se) || se.Status != 404 || se.Code != "not_found" {
		t.Fatalf("404: %v", err)
	}

	// A non-JSON error body still yields a StatusError with the status.
	status, body = 500, `boom`
	if _, _, err := c.Models(ctx); !errors.As(err, &se) || se.Status != 500 {
		t.Fatalf("500: %v", err)
	}
	// Redirects are not followed.
	status, body = 302, ``
	if _, _, err := c.Models(ctx); !errors.As(err, &se) || se.Status != 302 {
		t.Fatalf("302: %v", err)
	}
}
