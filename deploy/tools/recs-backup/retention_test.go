package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

// Security audit SA5-3: the site promises that backups holding a deleted
// account's data are gone within 5 weeks, so no daily or weekly copy older
// than 35 days survives a backup run, whatever --keep-* says.
func TestBackupsOlderThan35DaysPruned(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)
	now := day("2026-11-10")
	b := filepath.Join(dir, "b")
	files := map[string]bool{ // path -> kept
		filepath.Join(b, "daily", "app-2026-10-07.db"):  true,  // 34 days
		filepath.Join(b, "daily", "app-2026-10-06.db"):  false, // 35 days: nothing that old survives
		filepath.Join(b, "daily", "app-2026-10-05.db"):  false, // 36 days
		filepath.Join(b, "weekly", "app-2026-10-13.db"): true,
		filepath.Join(b, "weekly", "app-2026-10-01.db"): false,
		filepath.Join(b, "weekly", "app-2026-08-01.db"): false,
		// Dated in the future (clock stepped back, bad name) or not a date at all.
		filepath.Join(b, "daily", "app-2026-11-11.db"):  true,  // tomorrow: within the allowed skew
		filepath.Join(b, "daily", "app-2026-11-13.db"):  false, // three days ahead
		filepath.Join(b, "weekly", "app-2027-01-01.db"): false,
		filepath.Join(b, "daily", "app-2026-13-45.db"):  false, // matches the pattern, no such date
	}
	for p := range files {
		if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	// Generous counts, so only the age cap can remove anything.
	if _, err := backup(context.Background(), dbPath, b, 60, 60, now); err != nil {
		t.Fatal(err)
	}
	for p, want := range files {
		_, err := os.Stat(p)
		if kept := err == nil; kept != want {
			t.Errorf("%s: kept=%v, want %v", filepath.Base(filepath.Dir(p))+"/"+filepath.Base(p), kept, want)
		}
	}
}

// The newest copy in a directory is never deleted for its age (a restore
// must always have something to restore from).
func TestPruneKeepsNewestWhateverItsAge(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"app-2026-01-01.db", "app-2026-02-01.db"} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	removed, err := prune(dir, 7, day("2026-11-10"), "")
	if err != nil {
		t.Fatal(err)
	}
	if len(removed) != 1 || filepath.Base(removed[0]) != "app-2026-01-01.db" {
		t.Fatalf("removed %v", removed)
	}
	if _, err := os.Stat(filepath.Join(dir, "app-2026-02-01.db")); err != nil {
		t.Fatalf("newest copy removed: %v", err)
	}
}

// The clock stepped back three days: today's backup is written under an
// older date than the existing copies. They count as expired; the file just
// written is kept even though it is not the newest name.
func TestBackupAfterClockStepBack(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)
	b := filepath.Join(dir, "b")
	ahead := []string{filepath.Join(b, "daily", "app-2026-11-12.db"), filepath.Join(b, "daily", "app-2026-11-13.db")}
	for _, p := range ahead {
		if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	res, err := backup(context.Background(), dbPath, b, 7, 4, day("2026-11-10"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(res.Daily); err != nil {
		t.Fatalf("the backup just written was pruned: %v", err)
	}
	for _, p := range ahead {
		if _, err := os.Stat(p); !os.IsNotExist(err) {
			t.Errorf("%s (dated ahead of the clock) kept", filepath.Base(p))
		}
	}
	if got, err := latest(b); err != nil || filepath.Base(got) != filepath.Base(res.Daily) {
		t.Errorf("latest = %s, %v; want the backup just written", got, err)
	}
}
