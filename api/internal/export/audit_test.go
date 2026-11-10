package export

import (
	"bufio"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// Audit L2: the data file is published with a hard link, which fails if the
// target exists, even when the file appears between any check and the
// publish (simulated by creating it from inside the write).
func TestPublishNeverClobbers(t *testing.T) {
	dir := t.TempDir()
	final := filepath.Join(dir, "ratings-2026-11-10T03-00-05Z.csv")
	err := publishNew(dir, final, func(w *bufio.Writer) error {
		if err := os.WriteFile(final, []byte("sentinel"), 0o600); err != nil {
			return err
		}
		_, err := w.WriteString("data")
		return err
	})
	if err == nil {
		t.Fatal("publish over an existing file succeeded")
	}
	if b, _ := os.ReadFile(final); string(b) != "sentinel" {
		t.Fatalf("existing file overwritten: %q", b)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Fatalf("temp file left behind: %v", entries)
	}
}

// Audit L3: a directory writable by group or others is refused.
func TestRefusesLooseDirectory(t *testing.T) {
	d := cachetest.OpenDB(t)
	for _, mode := range []os.FileMode{0o770, 0o777, 0o702} {
		dir := filepath.Join(t.TempDir(), "exports")
		if err := os.Mkdir(dir, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.Chmod(dir, mode); err != nil {
			t.Fatal(err)
		}
		_, err := Export(context.Background(), d, dir, t0)
		if err == nil || !strings.Contains(err.Error(), "writable by group or others") {
			t.Errorf("mode %o: err = %v", mode, err)
		}
		if entries, _ := os.ReadDir(dir); len(entries) != 0 {
			t.Errorf("mode %o: wrote %v", mode, entries)
		}
	}
	for _, mode := range []os.FileMode{0o700, 0o750, 0o755} {
		dir := filepath.Join(t.TempDir(), "exports")
		if err := os.Mkdir(dir, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.Chmod(dir, mode); err != nil {
			t.Fatal(err)
		}
		if _, err := Export(context.Background(), d, dir, t0); err != nil {
			t.Errorf("mode %o: %v", mode, err)
		}
	}
	// A newly created directory is acceptable (0750 before umask).
	if _, err := Export(context.Background(), d, filepath.Join(t.TempDir(), "new", "exports"), t0); err != nil {
		t.Fatal(err)
	}
}

// Temp files left by a crashed export are removed once they are over an
// hour old; a fresh one (a concurrent export) is left alone.
func TestPrunesStaleTempFiles(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := t.TempDir()
	stale := filepath.Join(dir, ".export-123.tmp")
	fresh := filepath.Join(dir, ".export-456.tmp")
	other := filepath.Join(dir, ".other.tmp")
	for _, p := range []string{stale, fresh, other} {
		if err := os.WriteFile(p, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	old := time.Now().Add(-2 * time.Hour)
	for _, p := range []string{stale, other} {
		if err := os.Chtimes(p, old, old); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := Export(context.Background(), d, dir, t0); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Errorf("stale temp file kept: %v", err)
	}
	for _, p := range []string{fresh, other} {
		if _, err := os.Stat(p); err != nil {
			t.Errorf("%s removed: %v", filepath.Base(p), err)
		}
	}
}
