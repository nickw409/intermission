package export

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

var t0 = time.Date(2026, 11, 10, 3, 0, 5, 0, time.UTC)

func rate(t *testing.T, d *db.DB, uid, movie int64, rating float64, ts int64) {
	t.Helper()
	if _, err := d.Writer.Exec("INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)", uid, movie, rating, ts); err != nil {
		t.Fatal(err)
	}
}

func readFile(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func TestExportFormat(t *testing.T) {
	d := cachetest.OpenDB(t)
	carol, _ := cachetest.NewUser(t, d, "carol_secret_name")
	alice, _ := cachetest.NewUser(t, d, "alice_secret_name")
	cachetest.NewUser(t, d, "nobody_rated") // no ratings: absent
	rate(t, d, alice, 2571, 4.5, 1794256855)
	rate(t, d, alice, 1, 4, 1794256802)
	rate(t, d, carol, 318, 0.5, 1794256787)
	rate(t, d, carol, 260, 5, 1794256872)
	dir := filepath.Join(t.TempDir(), "exports")

	res, err := Export(context.Background(), d, dir, t0)
	if err != nil {
		t.Fatal(err)
	}
	wantName := "ratings-2026-11-10T03-00-05Z.csv"
	if filepath.Base(res.Path) != wantName || res.Rows != 4 || res.Users != 2 {
		t.Fatalf("res = %+v", res)
	}
	// carol was created first, so she has the lower ID.
	want := "site_user_id,movie_id,rating,timestamp\n" +
		itoa(carol) + ",260,5.0,1794256872\n" +
		itoa(carol) + ",318,0.5,1794256787\n" +
		itoa(alice) + ",1,4.0,1794256802\n" +
		itoa(alice) + ",2571,4.5,1794256855\n"
	if got := readFile(t, res.Path); got != want {
		t.Fatalf("file:\n%s\nwant:\n%s", got, want)
	}
	if got := readFile(t, filepath.Join(dir, "LATEST")); got != wantName {
		t.Fatalf("LATEST = %q", got)
	}
	st, err := os.Stat(res.Path)
	if err != nil || st.Mode().Perm()&0o007 != 0 {
		t.Fatalf("mode %v %v", st.Mode(), err)
	}
	// Nothing else (no temp files) is left behind.
	entries, _ := os.ReadDir(dir)
	var names []string
	for _, e := range entries {
		names = append(names, e.Name())
	}
	if !slices.Equal(names, []string{"LATEST", wantName}) {
		t.Fatalf("dir = %v", names)
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

// Deleted accounts are absent: their ratings cascade away before export.
func TestDeletedAccountsAbsent(t *testing.T) {
	d := cachetest.OpenDB(t)
	keep, _ := cachetest.NewUser(t, d, "keep")
	gone, _ := cachetest.NewUser(t, d, "gone_user")
	rate(t, d, keep, 10, 3, 1)
	rate(t, d, gone, 11, 2, 2)
	rate(t, d, gone, 12, 2, 3)
	if _, err := d.Writer.Exec("DELETE FROM users WHERE id = ?", gone); err != nil {
		t.Fatal(err)
	}
	res, err := Export(context.Background(), d, t.TempDir(), t0)
	if err != nil {
		t.Fatal(err)
	}
	got := readFile(t, res.Path)
	if got != "site_user_id,movie_id,rating,timestamp\n"+itoa(keep)+",10,3.0,1\n" {
		t.Fatalf("file:\n%s", got)
	}
}

func TestEmptyExportHasHeaderOnly(t *testing.T) {
	d := cachetest.OpenDB(t)
	res, err := Export(context.Background(), d, t.TempDir(), t0)
	if err != nil {
		t.Fatal(err)
	}
	if got := readFile(t, res.Path); got != "site_user_id,movie_id,rating,timestamp\n" || res.Rows != 0 {
		t.Fatalf("file %q res %+v", got, res)
	}
}

// Retention: the 7 newest exports are kept; other files are never touched.
func TestKeepsSevenNewest(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := t.TempDir()
	others := []string{"notes.txt", "ratings-bad.csv", "ratings-2026-11-01T00-00-00Z.csv.bak"}
	for _, n := range others {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	for i := 8; i >= 0; i-- { // oldest first, as the nightly job runs
		ts := t0.Add(-time.Duration(i+1) * 24 * time.Hour)
		if _, err := Export(context.Background(), d, dir, ts); err != nil {
			t.Fatal(err)
		}
	}
	res, err := Export(context.Background(), d, dir, t0)
	if err != nil {
		t.Fatal(err)
	}
	entries, _ := os.ReadDir(dir)
	var exports []string
	for _, e := range entries {
		if exportName.MatchString(e.Name()) {
			exports = append(exports, e.Name())
		}
	}
	if len(exports) != Keep || exports[len(exports)-1] != filepath.Base(res.Path) ||
		exports[0] != "ratings-2026-11-04T03-00-05Z.csv" {
		t.Fatalf("exports = %v", exports)
	}
	for _, n := range others {
		if _, err := os.Stat(filepath.Join(dir, n)); err != nil {
			t.Errorf("%s removed: %v", n, err)
		}
	}
	if got := readFile(t, filepath.Join(dir, "LATEST")); got != filepath.Base(res.Path) {
		t.Fatalf("LATEST = %q", got)
	}
}

// An export never overwrites an existing file of the same name.
func TestSameSecondDoesNotOverwrite(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := t.TempDir()
	first, err := Export(context.Background(), d, dir, t0)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(first.Path, []byte("sentinel"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Export(context.Background(), d, dir, t0); err == nil {
		t.Fatal("second export in the same second succeeded")
	}
	if readFile(t, first.Path) != "sentinel" {
		t.Fatal("existing export overwritten")
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 2 {
		t.Fatalf("leftover files: %v", entries)
	}
}

func TestRunFlags(t *testing.T) {
	d := cachetest.OpenDB(t)
	dir := filepath.Join(t.TempDir(), "out")
	deps := &app.Deps{Config: app.Config{ExportsDir: filepath.Join(t.TempDir(), "unused")}, DB: d,
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	if err := Run(context.Background(), deps, []string{"--dir", dir}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "LATEST")); err != nil {
		t.Fatal(err)
	}
	if err := Run(context.Background(), deps, []string{"--bogus"}); err == nil {
		t.Fatal("unknown flag accepted")
	}
	if err := Run(context.Background(), deps, []string{"extra"}); err == nil {
		t.Fatal("positional argument accepted")
	}
}
