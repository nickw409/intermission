package main

import (
	"bytes"
	"context"
	"database/sql"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// makeDB creates a WAL-mode database with a few rows, like recs-api's app.db.
func makeDB(t *testing.T, path string) {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+path+"?_pragma=journal_mode(WAL)")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	for _, q := range []string{
		"CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL)",
		"INSERT INTO users (username) VALUES ('a'), ('b'), ('c')",
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
}

func countUsers(t *testing.T, path string) int {
	t.Helper()
	db, err := openDB(path, true)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var n int
	if err := db.QueryRow("SELECT count(*) FROM users").Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func day(s string) time.Time {
	t, err := time.Parse(dateLayout, s)
	if err != nil {
		panic(err)
	}
	return t.Add(3 * time.Hour)
}

func TestBackupWritesVerifiedPrivateCopy(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)

	res, err := backup(context.Background(), dbPath, filepath.Join(dir, "b"), 7, 4, day("2026-11-10"))
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(dir, "b", "daily", "app-2026-11-10.db"); res.Daily != want {
		t.Fatalf("daily = %s, want %s", res.Daily, want)
	}
	if res.Weekly == "" {
		t.Fatal("first backup must also create a weekly copy")
	}
	for _, p := range []string{res.Daily, res.Weekly} {
		st, err := os.Stat(p)
		if err != nil {
			t.Fatal(err)
		}
		if st.Mode().Perm() != 0o600 {
			t.Errorf("%s mode = %v, want 0600", p, st.Mode().Perm())
		}
		if n := countUsers(t, p); n != 3 {
			t.Errorf("%s has %d users, want 3", p, n)
		}
	}
}

func TestBackupIsIdempotentPerDay(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)
	for i := 0; i < 2; i++ {
		if _, err := backup(context.Background(), dbPath, filepath.Join(dir, "b"), 7, 4, day("2026-11-10")); err != nil {
			t.Fatalf("run %d: %v", i, err)
		}
	}
	daily, _ := listBackups(filepath.Join(dir, "b", "daily"))
	weekly, _ := listBackups(filepath.Join(dir, "b", "weekly"))
	if len(daily) != 1 || len(weekly) != 1 {
		t.Fatalf("daily=%v weekly=%v, want one of each", daily, weekly)
	}
}

func TestRetentionKeepsSevenDailyAndFourWeekly(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)
	start := day("2026-01-01")
	for i := 0; i < 60; i++ {
		if _, err := backup(context.Background(), dbPath, filepath.Join(dir, "b"), 7, 4, start.AddDate(0, 0, i)); err != nil {
			t.Fatal(err)
		}
	}
	daily, _ := listBackups(filepath.Join(dir, "b", "daily"))
	weekly, _ := listBackups(filepath.Join(dir, "b", "weekly"))
	if len(daily) != 7 {
		t.Errorf("daily = %d files, want 7", len(daily))
	}
	if len(weekly) != 4 {
		t.Errorf("weekly = %d files, want 4", len(weekly))
	}
	// The newest daily is the last day; weekly copies are 7 days apart.
	if got := filepath.Base(daily[6]); got != "app-2026-03-01.db" {
		t.Errorf("newest daily = %s", got)
	}
	for i := 1; i < len(weekly); i++ {
		a, _ := backupDate(filepath.Base(weekly[i-1]))
		b, _ := backupDate(filepath.Base(weekly[i]))
		if b.Sub(a) != 7*24*time.Hour {
			t.Errorf("weekly gap %s -> %s", weekly[i-1], weekly[i])
		}
	}
}

func TestIntegrityCheckRejectsCorruptFile(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.db")
	if err := os.WriteFile(bad, bytes.Repeat([]byte("not a database "), 512), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := integrityCheck(context.Background(), bad); err == nil {
		t.Fatal("corrupt file passed the integrity check")
	}
}

func TestRestoreRefusesOverwriteWithoutForce(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "app.db")
	makeDB(t, dbPath)
	res, err := backup(context.Background(), dbPath, filepath.Join(dir, "b"), 7, 4, day("2026-11-10"))
	if err != nil {
		t.Fatal(err)
	}
	if err := restore(context.Background(), res.Daily, dbPath, false); err == nil || !strings.Contains(err.Error(), "--force") {
		t.Fatalf("restore over existing db without --force: err = %v", err)
	}
	target := filepath.Join(dir, "restored", "app.db")
	if err := os.MkdirAll(filepath.Dir(target), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := restore(context.Background(), res.Daily, target, false); err != nil {
		t.Fatal(err)
	}
	if n := countUsers(t, target); n != 3 {
		t.Fatalf("restored db has %d users, want 3", n)
	}
}

func TestLatestPicksNewestAcrossDailyAndWeekly(t *testing.T) {
	dir := t.TempDir()
	for _, p := range []string{"daily/app-2026-11-08.db", "daily/app-2026-11-09.db", "weekly/app-2026-11-02.db", "daily/notes.txt"} {
		full := filepath.Join(dir, p)
		if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, nil, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	got, err := latest(dir)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(got) != "app-2026-11-09.db" {
		t.Fatalf("latest = %s", got)
	}
	if _, err := latest(t.TempDir()); err == nil {
		t.Fatal("latest on an empty dir must fail")
	}
}

func TestCLIExitCodes(t *testing.T) {
	var out, errb bytes.Buffer
	now := func() time.Time { return day("2026-11-10") }
	if code := run(context.Background(), nil, &out, &errb, now); code != 2 {
		t.Errorf("no args: exit %d, want 2", code)
	}
	if code := run(context.Background(), []string{"backup"}, &out, &errb, now); code != 2 {
		t.Errorf("backup without flags: exit %d, want 2", code)
	}
	if code := run(context.Background(), []string{"backup", "--db", "/nonexistent/app.db", "--dir", t.TempDir()}, &out, &errb, now); code != 1 {
		t.Errorf("backup of a missing db: exit %d, want 1", code)
	}
}
