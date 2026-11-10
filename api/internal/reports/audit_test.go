package reports

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// Audit test gap: a valid top-level <run_id>.json written after the current
// listing was built is not served until the listing is rebuilt; the lookup
// never falls back to the file system.
func TestUnlistedFileNamedLikeRunIDNotServed(t *testing.T) {
	l := newLayout(t)
	e := newEnv(t, l)
	if rec := e.do("/api/reports"); rec.Code != 200 {
		t.Fatal(rec.Code)
	}
	write(t, filepath.Join(l.reports, "fresh_run.json"), report("fresh_run", privateMarker))
	e.clock.advance(listingTTL / 2)
	if rec := e.do("/api/reports/fresh_run"); rec.Code != 404 || strings.Contains(rec.Body.String(), privateMarker) {
		t.Fatalf("unlisted report served: %d %s", rec.Code, rec.Body.String())
	}
}

// Audit info: a FIFO named *.json is skipped without blocking the listing,
// and openRegular refuses FIFOs and symlinks.
func TestOpenRegularRefusesFIFOAndSymlink(t *testing.T) {
	l := newLayout(t)
	fifo := filepath.Join(l.reports, "pipe.json")
	if err := syscall.Mkfifo(fifo, 0o644); err != nil {
		t.Skip("mkfifo:", err)
	}
	e := newEnv(t, l)
	done := make(chan int, 1)
	go func() { done <- e.do("/api/reports").Code }()
	select {
	case code := <-done:
		if code != 200 {
			t.Fatal(code)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("listing blocked on a FIFO")
	}
	if f, err := openRegular(fifo); err == nil {
		f.Close()
		t.Fatal("openRegular accepted a FIFO")
	}
	link := filepath.Join(l.reports, "link.json")
	if err := os.Symlink(filepath.Join(l.private, "secret_run.json"), link); err != nil {
		t.Fatal(err)
	}
	if f, err := openRegular(link); err == nil {
		f.Close()
		t.Fatal("openRegular followed a symlink")
	}
	if f, err := openRegular(filepath.Join(l.reports, "als.json")); err != nil {
		t.Fatalf("regular file refused: %v", err)
	} else {
		f.Close()
	}
}
