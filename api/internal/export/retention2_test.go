package export

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// PR #42 review L1: retention protects the file LATEST names and the one
// just written explicitly, and does not trust names it cannot date.

func writeFiles(t *testing.T, dir string, names ...string) {
	t.Helper()
	for _, n := range names {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
}

func exists(dir, name string) bool {
	_, err := os.Stat(filepath.Join(dir, name))
	return err == nil
}

func TestPruneProtectsLatestAndExpiresUndatable(t *testing.T) {
	dir := t.TempDir()
	latest := exportFile(t0.Add(-20 * 24 * time.Hour)) // what recs-train reads
	fresh := exportFile(t0.Add(-24 * time.Hour))
	bogus := "ratings-2026-13-45T99-99-99Z.csv" // matches the pattern, is no time
	writeFiles(t, dir, latest, fresh, bogus)
	if err := os.WriteFile(filepath.Join(dir, LatestName), []byte(latest), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := prune(dir, t0, ""); err != nil {
		t.Fatal(err)
	}
	if !exists(dir, latest) {
		t.Error("the export LATEST names was pruned")
	}
	if !exists(dir, fresh) {
		t.Error("a day-old export was pruned")
	}
	if exists(dir, bogus) {
		t.Error("an export whose name has no valid time was kept")
	}
}

// PR #42 re-review 1: the clock stepped back three days, so every existing
// export looks future-dated. They are real recent exports: kept, reported,
// and the one just written survives too.
func TestExportAfterClockStepBack(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := t.TempDir()
	ahead := []string{exportFile(t0.Add(2 * 24 * time.Hour)), exportFile(t0.Add(3 * 24 * time.Hour))}
	writeFiles(t, dir, ahead...)
	res, err := Export(context.Background(), d, dir, t0)
	if err != nil {
		t.Fatal(err)
	}
	if !exists(dir, filepath.Base(res.Path)) {
		t.Fatal("the export just written was pruned")
	}
	for _, n := range ahead {
		if !exists(dir, n) {
			t.Errorf("export dated %s (ahead of the clock) deleted", n)
		}
	}
	if len(res.FutureDated) != 2 {
		t.Errorf("FutureDated = %v, want both", res.FutureDated)
	}
}

// PR #42 re-review 3: a LATEST that names a missing or non-export file does
// not use up one of the Keep slots of real exports.
func TestBrokenLatestDoesNotReduceKept(t *testing.T) {
	for _, target := range []string{"ratings-2026-01-01T00-00-00Z.csv", "notes.txt"} {
		dir := t.TempDir()
		var names []string
		for i := 1; i <= Keep; i++ {
			names = append(names, exportFile(t0.Add(-time.Duration(i)*time.Hour)))
		}
		writeFiles(t, dir, names...)
		writeFiles(t, dir, "notes.txt")
		if err := os.WriteFile(filepath.Join(dir, LatestName), []byte(target), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := prune(dir, t0, ""); err != nil {
			t.Fatal(err)
		}
		for _, n := range names {
			if !exists(dir, n) {
				t.Errorf("LATEST=%s: export %s pruned although only %d exist", target, n, Keep)
			}
		}
	}
}

func runDeps(t *testing.T) *app.Deps {
	t.Helper()
	return &app.Deps{DB: cachetest.OpenDB(t), Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
}

// A failed export still prunes what retention says must go.
func TestRunPrunesEvenWhenExportFails(t *testing.T) {
	deps := runDeps(t)
	dir := t.TempDir()
	old := exportFile(time.Now().Add(-10 * 24 * time.Hour))
	latest := exportFile(time.Now().Add(-9 * 24 * time.Hour))
	writeFiles(t, dir, old, latest)
	if err := os.WriteFile(filepath.Join(dir, LatestName), []byte(latest), 0o600); err != nil {
		t.Fatal(err)
	}
	_ = deps.DB.Close() // the export cannot read the database
	if err := Run(context.Background(), deps, []string{"--dir", dir}); err == nil {
		t.Fatal("export with a closed database succeeded")
	}
	if exists(dir, old) {
		t.Error("10-day-old export survived a failed run")
	}
	if !exists(dir, latest) {
		t.Error("the export LATEST names was pruned")
	}
}

// The nightly export run also prunes pre-migration backups and truncates the
// WAL (security audit SA5-3: deleted data must not wait for the next
// startup or migration).
func TestRunNightlyHousekeeping(t *testing.T) {
	deps := runDeps(t)
	dbDir := filepath.Dir(deps.DB.Path())
	backups := filepath.Join(dbDir, "backups")
	if err := os.MkdirAll(backups, 0o700); err != nil {
		t.Fatal(err)
	}
	stale := "app-pre-migrate-v2-" + time.Now().Add(-40*24*time.Hour).UTC().Format("20060102T150405.000000000Z") + ".db"
	writeFiles(t, backups, stale)
	if _, err := deps.DB.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('walfill', 'x', 0)"); err != nil {
		t.Fatal(err)
	}
	if err := Run(context.Background(), deps, []string{"--dir", t.TempDir()}); err != nil {
		t.Fatal(err)
	}
	if exists(backups, stale) {
		t.Error("40-day-old pre-migration backup survived the nightly run")
	}
	st, err := os.Stat(deps.DB.Path() + "-wal")
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 0 {
		t.Errorf("WAL is %d bytes after the nightly run", st.Size())
	}
}
