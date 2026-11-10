package export

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

func exportFile(at time.Time) string {
	return "ratings-" + at.UTC().Format("2006-01-02T15-04-05Z") + ".csv"
}

// Security audit SA5-3: an export holding a deleted account's ratings is gone
// within a week even when fewer than Keep exports exist (e.g. after the
// nightly job was paused and resumed).
func TestExportsOlderThanAWeekPruned(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := t.TempDir()
	ages := map[time.Duration]bool{ // age -> kept; negative = dated in the future
		2 * 24 * time.Hour:           true,
		7*24*time.Hour - time.Second: true,
		7 * 24 * time.Hour:           false, // nothing a week old survives
		30 * 24 * time.Hour:          false,
		-time.Hour:                   true, // within the allowed clock skew
		-2 * 24 * time.Hour:          true, // dated ahead: the clock stepped back; kept, warned about
	}
	for age := range ages {
		if err := os.WriteFile(filepath.Join(dir, exportFile(t0.Add(-age))), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	res, err := Export(context.Background(), d, dir, t0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(res.Path); err != nil {
		t.Fatalf("new export missing: %v", err)
	}
	for age, want := range ages {
		_, err := os.Stat(filepath.Join(dir, exportFile(t0.Add(-age))))
		if kept := err == nil; kept != want {
			t.Errorf("export aged %v: kept=%v, want %v", age, kept, want)
		}
	}
}

// The newest export is never deleted for its age: it is what LATEST names
// and what recs-train reads.
func TestPruneKeepsNewestExportWhateverItsAge(t *testing.T) {
	dir := t.TempDir()
	older := exportFile(t0.Add(-40 * 24 * time.Hour))
	newest := exportFile(t0.Add(-30 * 24 * time.Hour))
	for _, n := range []string{older, newest} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := prune(dir, t0, ""); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, newest)); err != nil {
		t.Errorf("newest export removed: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, older)); !os.IsNotExist(err) {
		t.Errorf("old export kept: %v", err)
	}
}
