package db

import (
	"bytes"
	"context"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"testing/fstest"
	"time"
)

// Security audit fixes SA1-1 (user IDs never reused) and SA5-3 (deleted
// account residue).

// embeddedUpTo returns the first n embedded migrations as their own FS.
func embeddedUpTo(t *testing.T, n int) fstest.MapFS {
	t.Helper()
	ms, err := loadMigrations(embedded())
	if err != nil {
		t.Fatal(err)
	}
	out := fstest.MapFS{}
	for _, m := range ms[:n] {
		out[m.name] = &fstest.MapFile{Data: []byte(m.sql)}
	}
	return out
}

func mustExec(t *testing.T, d *DB, q string, args ...any) {
	t.Helper()
	if _, err := d.Writer.Exec(q, args...); err != nil {
		t.Fatalf("%s: %v", q, err)
	}
}

func (d *DB) countRows(t *testing.T, q string, args ...any) int {
	t.Helper()
	var n int
	if err := d.Reader.QueryRow(q, args...).Scan(&n); err != nil {
		t.Fatalf("%s: %v", q, err)
	}
	return n
}

// The users rebuild (AUTOINCREMENT) applied to a database that already holds
// accounts keeps every row, keeps ON DELETE CASCADE working, and leaves
// foreign keys enforced.
func TestUsersRebuildOnExistingRows(t *testing.T) {
	d, _ := openTemp(t)
	ctx := context.Background()
	if err := d.migrate(ctx, quietLogger(), embeddedUpTo(t, 2)); err != nil {
		t.Fatal(err)
	}
	now := time.Now().Unix()
	for i, name := range []string{"alice", "bob", "carol"} {
		mustExec(t, d, "INSERT INTO users (username, pw_hash, created_at, ratings_version) VALUES (?, 'h', ?, ?)", name, now, i+1)
	}
	for uid := 1; uid <= 3; uid++ {
		mustExec(t, d, "INSERT INTO sessions VALUES (?, ?, ?, ?)", []byte{byte(uid)}, uid, now, now+60)
		mustExec(t, d, "INSERT INTO ratings VALUES (?, 10, 4.5, ?)", uid, now)
		mustExec(t, d, "INSERT INTO feedback VALUES (?, 10, 'pop', 'click', ?)", uid, now)
	}
	mustExec(t, d, "DELETE FROM users WHERE id = 3")

	if err := d.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	if v, _ := d.Version(ctx); v != latestVersion(t) {
		t.Fatalf("version = %d", v)
	}
	var schema string
	if err := d.Reader.QueryRow("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'users'").Scan(&schema); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.ToUpper(schema), "AUTOINCREMENT") {
		t.Fatalf("users is not AUTOINCREMENT:\n%s", schema)
	}
	if got := pragma(t, d.Writer, "foreign_keys"); got != "1" {
		t.Fatalf("writer foreign_keys = %s after migrating", got)
	}
	if n := d.countRows(t, "SELECT count(*) FROM pragma_foreign_key_check"); n != 0 {
		t.Fatalf("%d foreign key violations", n)
	}
	// Rows survived, children included (a DROP with foreign keys on would
	// have cascaded them away).
	var name string
	var rv int
	if err := d.Reader.QueryRow("SELECT username, ratings_version FROM users WHERE id = 2").Scan(&name, &rv); err != nil || name != "bob" || rv != 2 {
		t.Fatalf("user 2 = %q %d %v", name, rv, err)
	}
	for _, table := range []string{"users", "sessions", "ratings", "feedback"} {
		if n := d.countRows(t, "SELECT count(*) FROM "+table); n != 2 { // sqlguard:allow fixed table names
			t.Errorf("%s has %d rows after the rebuild, want 2", table, n)
		}
	}
	// Indexes on the child tables still exist.
	for _, idx := range []string{"sessions_user_id", "sessions_expires_at", "feedback_user_id"} {
		if n := d.countRows(t, "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = ?", idx); n != 1 {
			t.Errorf("index %s missing", idx)
		}
	}
	// Spec constraints survived the rebuild.
	if _, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('ALICE', 'h', 0)"); err == nil {
		t.Error("usernames are no longer case-insensitively unique")
	}
	if _, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('ab', 'h', 0)"); err == nil {
		t.Error("username length check lost")
	}
	if _, err := d.Writer.Exec("INSERT INTO sessions VALUES (x'ff', 999, 0, 0)"); err == nil {
		t.Error("foreign key not enforced after the rebuild")
	}
	// Every foreign key that references users still cascades.
	mustExec(t, d, "DELETE FROM users WHERE id = 1")
	for _, table := range []string{"sessions", "ratings", "feedback"} {
		if n := d.countRows(t, "SELECT count(*) FROM "+table+" WHERE user_id = 1"); n != 0 { // sqlguard:allow fixed table names
			t.Errorf("%s rows of a deleted user survived", table)
		}
	}
	// IDs only move forward from here on.
	res, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('dave', 'h', 0)")
	if err != nil {
		t.Fatal(err)
	}
	dave, _ := res.LastInsertId()
	if dave <= 2 {
		t.Fatalf("new user got id %d, at or below an existing/deleted one", dave)
	}
	mustExec(t, d, "DELETE FROM users WHERE id = ?", dave)
	res, err = d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES ('erin', 'h', 0)")
	if err != nil {
		t.Fatal(err)
	}
	if erin, _ := res.LastInsertId(); erin <= dave {
		t.Fatalf("deleted id %d reused (got %d)", dave, erin)
	}
}

// A migration that leaves a dangling reference is refused as a whole.
func TestMigrateRefusesForeignKeyViolations(t *testing.T) {
	d, _ := openTemp(t)
	ctx := context.Background()
	v1 := fstest.MapFS{"0001_a.sql": {Data: []byte("CREATE TABLE p (id INTEGER PRIMARY KEY); CREATE TABLE c (pid INTEGER REFERENCES p(id));")}}
	if err := d.migrate(ctx, quietLogger(), v1); err != nil {
		t.Fatal(err)
	}
	v2 := fstest.MapFS{
		"0001_a.sql": v1["0001_a.sql"],
		"0002_b.sql": {Data: []byte("INSERT INTO c VALUES (7);")},
	}
	if err := d.migrate(ctx, quietLogger(), v2); err == nil {
		t.Fatal("migration with a dangling foreign key committed")
	}
	if v, _ := d.Version(ctx); v != 1 {
		t.Fatalf("version = %d", v)
	}
	if got := pragma(t, d.Writer, "foreign_keys"); got != "1" {
		t.Fatalf("writer foreign_keys = %s after a failed migration", got)
	}
}

// SA5-3: after a checkpoint the WAL (which still holds deleted pages) is
// truncated to zero bytes.
func TestCheckpointTruncatesWAL(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	if err := d.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	mustExec(t, d, "INSERT INTO users (username, pw_hash, created_at) VALUES ('alice', 'h', 0)")
	mustExec(t, d, "DELETE FROM users")
	if st, err := os.Stat(path + "-wal"); err != nil || st.Size() == 0 {
		t.Fatalf("expected a non-empty WAL before the checkpoint: %v", err)
	}
	if err := d.CheckpointTruncate(ctx); err != nil {
		t.Fatal(err)
	}
	st, err := os.Stat(path + "-wal")
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 0 {
		t.Fatalf("WAL is %d bytes after TRUNCATE checkpoint", st.Size())
	}
}

func backupFileName(version int, at time.Time) string {
	return fmt.Sprintf("app-pre-migrate-v%d-%s.db", version, at.UTC().Format(backupStampLayout))
}

// SA5-3: pre-migration backups are pruned by count and by age, but one
// younger than a day is never deleted. Ages come from the name's timestamp,
// not the version (v10 sorts before v9 as text).
func TestPruneBackupsRetention(t *testing.T) {
	now := time.Date(2026, 11, 10, 12, 0, 0, 0, time.UTC)
	day := 24 * time.Hour
	cases := []struct {
		name string
		ages map[string]time.Duration // label -> age
		want []string                 // labels kept
	}{
		{"count cap", map[string]time.Duration{"a": 2 * day, "b": 3 * day, "c": 4 * day, "d": 5 * day, "e": 6 * day},
			[]string{"a", "b", "c"}},
		{"age cap even within the newest 3", map[string]time.Duration{"a": 2 * day, "b": 36 * day, "c": 90 * day},
			[]string{"a"}},
		// Nothing older than 35 days survives a run.
		{"exactly 35 days is deleted", map[string]time.Duration{"a": 35 * day},
			nil},
		{"just under 35 days is kept", map[string]time.Duration{"a": 35*day - time.Minute},
			[]string{"a"}},
		{"never delete one younger than a day", map[string]time.Duration{"a": time.Hour, "b": 2 * time.Hour, "c": 3 * time.Hour, "d": 4 * time.Hour, "e": 23 * time.Hour},
			[]string{"a", "b", "c", "d", "e"}},
		{"old lone backup goes", map[string]time.Duration{"a": 40 * day},
			nil},
		// Negative ages are in the future: most likely the clock stepped back, so these are
		// real, recent backups. Never deleted, not counted towards the newest 3, warned about.
		{"dated more than a day ahead is kept and not counted", map[string]time.Duration{
			"a": -2 * day, "b": -30 * day, "c": 2 * day, "d": 3 * day, "e": 4 * day, "f": 5 * day},
			[]string{"a", "b", "c", "d", "e"}},
		{"dated less than a day ahead is kept", map[string]time.Duration{"a": -time.Hour},
			[]string{"a"}},
		{"clock stepped back: everything survives", map[string]time.Duration{"a": -2 * day, "b": -3 * day, "protected": -4 * day},
			[]string{"a", "b", "protected"}},
		{"the file just written is kept whatever its date", map[string]time.Duration{"protected": 90 * day},
			[]string{"protected"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			names := map[string]string{}
			v := 10
			for label, age := range tc.ages {
				names[label] = backupFileName(v, now.Add(-age))
				v-- // versions deliberately out of age order
				if err := os.WriteFile(filepath.Join(dir, names[label]), []byte("x"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			unrelated := []string{"app-2026-01-01.db", "app-pre-migrate-notes.txt", "app-pre-migrate-v1-garbage.db"}
			for _, n := range unrelated {
				if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			// Matches the name pattern but is no valid time: expired.
			bogus := "app-pre-migrate-v1-20261399T999999.000000000Z.db"
			if err := os.WriteFile(filepath.Join(dir, bogus), []byte("x"), 0o600); err != nil {
				t.Fatal(err)
			}
			future, err := pruneBackups(dir, now, names["protected"])
			if err != nil {
				t.Fatal(err)
			}
			for label, age := range tc.ages {
				if want := age < -clockSkew && label != "protected"; contains(future, names[label]) != want {
					t.Errorf("%s (age %v): reported as future-dated=%v, want %v", label, age, !want, want)
				}
			}
			if _, err := os.Stat(filepath.Join(dir, bogus)); !os.IsNotExist(err) {
				t.Errorf("unparsable backup name kept: %v", err)
			}
			for label, n := range names {
				_, err := os.Stat(filepath.Join(dir, n))
				kept := err == nil
				if want := contains(tc.want, label); kept != want {
					t.Errorf("%s (age %v): kept=%v, want %v", label, tc.ages[label], kept, want)
				}
			}
			for _, n := range unrelated {
				if _, err := os.Stat(filepath.Join(dir, n)); err != nil {
					t.Errorf("unrelated file %s removed", n)
				}
			}
		})
	}
}

// SA5-3: a migration prunes old pre-migration backups after writing its own.
func TestPreMigrationBackupsPruned(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	v1 := fstest.MapFS{"0001_a.sql": {Data: []byte("CREATE TABLE a (x INTEGER);")}}
	if err := d.migrate(ctx, quietLogger(), v1); err != nil {
		t.Fatal(err)
	}
	mustExec(t, d, "INSERT INTO a VALUES (1)")
	dir := filepath.Join(filepath.Dir(path), "backups")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	// Older backups, oldest first.
	now := time.Now()
	old := []string{
		backupFileName(10, now.Add(-5*24*time.Hour)),
		backupFileName(9, now.Add(-4*24*time.Hour)),
		backupFileName(1, now.Add(-3*24*time.Hour)),
		backupFileName(2, now.Add(-2*24*time.Hour)),
	}
	keep := []string{"nightly-20260101.db", "app-pre-migrate-notes.txt"}
	for _, n := range append(append([]string{}, old...), keep...) {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	v2 := fstest.MapFS{
		"0001_a.sql": v1["0001_a.sql"],
		"0002_b.sql": {Data: []byte("CREATE TABLE b (y INTEGER);")},
	}
	if err := d.migrate(ctx, quietLogger(), v2); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, e := range entries {
		got = append(got, e.Name())
	}
	sort.Strings(got)
	var pre []string
	for _, n := range got {
		if strings.HasPrefix(n, "app-pre-migrate-v") && strings.HasSuffix(n, ".db") {
			pre = append(pre, n)
		}
	}
	if len(pre) != 3 {
		t.Fatalf("pre-migration backups = %v, want 3", pre)
	}
	for _, n := range []string{old[2], old[3]} {
		if !contains(pre, n) {
			t.Errorf("newest backup %s was pruned (have %v)", n, pre)
		}
	}
	for _, n := range []string{old[0], old[1]} {
		if contains(pre, n) {
			t.Errorf("old backup %s kept", n)
		}
	}
	for _, n := range keep {
		if !contains(got, n) {
			t.Errorf("unrelated file %s removed", n)
		}
	}
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

// PR #42 re-review 1: a backup written while the clock was ahead is not
// deleted on the next startup; the startup log warns about it (file names
// only). Startup also truncates the WAL (re-review 6).
func TestStartupKeepsFutureDatedBackupAndCheckpoints(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	if err := d.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(filepath.Dir(path), "backups")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	ahead := backupFileName(2, time.Now().Add(3*24*time.Hour))
	if err := os.WriteFile(filepath.Join(dir, ahead), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	mustExec(t, d, "INSERT INTO users (username, pw_hash, created_at) VALUES ('walfill', 'h', 0)")
	var logs bytes.Buffer
	if err := d.Migrate(ctx, slog.New(slog.NewTextHandler(&logs, nil))); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, ahead)); err != nil {
		t.Fatalf("future-dated backup deleted at startup: %v", err)
	}
	if !strings.Contains(logs.String(), "clock") || !strings.Contains(logs.String(), ahead) {
		t.Errorf("no warning about the future-dated backup: %s", logs.String())
	}
	if strings.Contains(logs.String(), "walfill") {
		t.Errorf("user data in the log: %s", logs.String())
	}
	st, err := os.Stat(path + "-wal")
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 0 {
		t.Errorf("WAL is %d bytes after startup", st.Size())
	}
}

// PR #42 review H2: retention also runs when there is nothing to migrate,
// so every startup prunes, not just the rare one that migrates.
func TestUpToDateMigratePrunesOldBackups(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	if err := d.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(filepath.Dir(path), "backups")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	old := filepath.Join(dir, backupFileName(2, time.Now().Add(-40*24*time.Hour)))
	recent := filepath.Join(dir, backupFileName(2, time.Now().Add(-2*24*time.Hour)))
	for _, p := range []string{old, recent} {
		if err := os.WriteFile(p, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := d.Migrate(ctx, quietLogger()); err != nil { // up to date: no backup, no migration
		t.Fatal(err)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Errorf("40-day-old pre-migration backup survived an up-to-date Migrate: %v", err)
	}
	if _, err := os.Stat(recent); err != nil {
		t.Errorf("recent backup removed: %v", err)
	}
	// No backups directory at all is fine.
	d2, _ := openTemp(t)
	if err := d2.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	if err := d2.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
}
