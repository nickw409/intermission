package modelclient_test

import (
	"bytes"
	"context"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
)

type lockedBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuf) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}
func (l *lockedBuf) String() string { l.mu.Lock(); defer l.mu.Unlock(); return l.b.String() }

// Audit L1: the request URL (with ?q=) never reaches the logs or the
// returned error, whether the call times out, the connection drops or the
// socket is missing. Logged at debug level so nothing is filtered out.
func TestQueryNeverInErrorsOrLogs(t *testing.T) {
	const secret = "zebra-secret-query"
	logs := &lockedBuf{}
	logger := slog.New(slog.NewJSONHandler(logs, &slog.HandlerOptions{Level: slog.LevelDebug}))

	f := newFake(t)
	f.SetLatency(3 * time.Second)
	hang, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: fake.Handler(f), ReadHeaderTimeout: time.Second}
	go func() { _ = srv.Serve(hang) }()
	t.Cleanup(func() { _ = srv.Close() })

	drop, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	dl := &countingListener{Listener: drop, fail: 1 << 30}
	go func() {
		for {
			if _, err := dl.Accept(); err != nil {
				return
			}
		}
	}()
	t.Cleanup(func() { _ = drop.Close() })

	clients := map[string]modelclient.Client{
		"timeout":   modelclient.NewHTTP(modelclient.HTTPConfig{Addr: hang.Addr().String(), Timeout: 100 * time.Millisecond, Logger: logger}),
		"dropped":   modelclient.NewHTTP(modelclient.HTTPConfig{Addr: drop.Addr().String(), Logger: logger}),
		"no socket": modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: t.TempDir() + "/none.sock", Logger: logger}),
	}
	for name, c := range clients {
		_, _, err := c.Search(context.Background(), secret, 3)
		if !errors.Is(err, modelclient.ErrUnavailable) {
			t.Fatalf("%s: err = %v", name, err)
		}
		if strings.Contains(err.Error(), secret) || strings.Contains(err.Error(), "q=") {
			t.Errorf("%s: error leaks the query: %v", name, err)
		}
	}
	out := logs.String()
	if out == "" {
		t.Fatal("nothing logged at debug level")
	}
	if strings.Contains(out, secret) || strings.Contains(out, "q=") {
		t.Fatalf("logs leak the query:\n%s", out)
	}
}
