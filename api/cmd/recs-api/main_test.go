package main

import (
	"bytes"
	"context"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"
)

// syncBuffer is a bytes.Buffer safe for the server goroutine and the test.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

func env(m map[string]string) func(string) string { return func(k string) string { return m[k] } }

func TestUsageAndUnknown(t *testing.T) {
	var out, errb bytes.Buffer
	if code := run(context.Background(), nil, env(nil), &out, &errb, nil); code != 2 {
		t.Fatalf("no args: code %d", code)
	}
	if code := run(context.Background(), []string{"bogus"}, env(nil), &out, &errb, nil); code != 2 {
		t.Fatalf("bogus: code %d", code)
	}
	if code := run(context.Background(), []string{"help"}, env(nil), &out, &errb, nil); code != 0 || !strings.Contains(out.String(), "serve") {
		t.Fatalf("help: code %d out %q", code, out.String())
	}
}

func TestInvalidConfigFailsFast(t *testing.T) {
	var out, errb bytes.Buffer
	code := run(context.Background(), []string{"serve"}, env(map[string]string{"RECS_SIGNUP_MODE": "closed"}), &out, &errb, nil)
	if code != 1 || !strings.Contains(errb.String(), "RECS_SIGNUP_MODE") {
		t.Fatalf("code %d stderr %q", code, errb.String())
	}
}

func TestStubSubcommands(t *testing.T) {
	for _, args := range [][]string{{"migrate"}, {"export-ratings"}, {"admin", "invite", "create"}} {
		var out, errb bytes.Buffer
		code := run(context.Background(), args, env(nil), &out, &errb, nil)
		if code != 1 || !strings.Contains(errb.String(), "not implemented yet") {
			t.Errorf("%v: code %d stderr %q", args, code, errb.String())
		}
	}
}

func TestServeAndGracefulShutdown(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var out, errb syncBuffer
	addrc := make(chan string, 1)
	done := make(chan int, 1)
	go func() {
		done <- run(ctx, []string{"serve"}, env(map[string]string{"RECS_ADDR": "127.0.0.1:0", "TMDB_API_KEY": "do-not-log"}),
			&out, &errb, func(a string) { addrc <- a })
	}()
	var addr string
	select {
	case addr = <-addrc:
	case code := <-done:
		t.Fatalf("serve exited early with %d: %s", code, errb.String())
	case <-time.After(5 * time.Second):
		t.Fatal("serve did not start")
	}

	resp, err := http.Get("http://" + addr + "/api/nothing-registered-yet")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound || resp.Header.Get("X-Request-ID") == "" {
		t.Fatalf("status %d, X-Request-ID %q", resp.StatusCode, resp.Header.Get("X-Request-ID"))
	}

	cancel()
	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code %d: %s", code, errb.String())
		}
	case <-time.After(15 * time.Second):
		t.Fatal("serve did not shut down")
	}
	logs := errb.String()
	if strings.Contains(logs, "do-not-log") {
		t.Fatal("TMDB key logged")
	}
	if !strings.Contains(logs, `"route":"unmatched"`) || !strings.Contains(logs, "shutting down") {
		t.Fatalf("logs = %s", logs)
	}
}
