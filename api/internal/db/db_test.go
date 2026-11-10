package db

import (
	"bytes"
	"context"
	"database/sql"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"testing/fstest"
	"time"
)

func quietLogger() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func openTemp(t *testing.T) (*DB, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "app.db")
	d, err := Open(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = d.Close() })
	return d, path
}

func pragma(t *testing.T, conn *sql.DB, name string) string {
	t.Helper()
	var v string
	if err := conn.QueryRow("PRAGMA " + name).Scan(&v); err != nil {
		t.Fatalf("PRAGMA %s: %v", name, err)
	}
	return v
}

func TestOpenPoolsAndPragmas(t *testing.T) {
	d, _ := openTemp(t)
	if d.Writer == nil || d.Reader == nil || d.Writer == d.Reader {
		t.Fatal("want two distinct pools")
	}
	if got := d.Writer.Stats().MaxOpenConnections; got != 1 {
		t.Fatalf("writer MaxOpenConnections = %d, want 1", got)
	}
	if got := d.Reader.Stats().MaxOpenConnections; got < 2 {
		t.Fatalf("reader MaxOpenConnections = %d, want a pool", got)
	}
	for _, pool := range []*sql.DB{d.Writer, d.Reader} {
		if got := pragma(t, pool, "journal_mode"); got != "wal" {
			t.Errorf("journal_mode = %q", got)
		}
		if got := pragma(t, pool, "foreign_keys"); got != "1" {
			t.Errorf("foreign_keys = %q", got)
		}
		if got := pragma(t, pool, "busy_timeout"); got != "5000" {
			t.Errorf("busy_timeout = %q", got)
		}
	}
	// The reader pool cannot write.
	if _, err := d.Reader.Exec("CREATE TABLE nope (x INTEGER)"); err == nil {
		t.Fatal("reader pool accepted a write")
	}
}

// latestVersion is the number of embedded migrations.
func latestVersion(t *testing.T) int {
	t.Helper()
	ms, err := loadMigrations(embedded())
	if err != nil {
		t.Fatal(err)
	}
	return len(ms)
}

// columns returns "name type notnull dflt pk" for every column of table.
func columns(t *testing.T, conn *sql.DB, table string) []string {
	t.Helper()
	rows, err := conn.Query("SELECT name, type, \"notnull\", coalesce(dflt_value, ''), pk FROM pragma_table_info(?)", table)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var name, typ, dflt string
		var notnull, pk int
		if err := rows.Scan(&name, &typ, &notnull, &dflt, &pk); err != nil {
			t.Fatal(err)
		}
		out = append(out, strings.Join([]string{name, typ, strconv.Itoa(notnull), dflt, strconv.Itoa(pk)}, " "))
	}
	return out
}

func TestMigrateFreshSchemaMatchesSpec(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	if err := d.Migrate(ctx, quietLogger()); err != nil {
		t.Fatal(err)
	}
	if v, err := d.Version(ctx); err != nil || v != latestVersion(t) {
		t.Fatalf("version = %d, %v", v, err)
	}
	want := map[string][]string{
		"users": {
			"id INTEGER 0  1", "username TEXT 1  0", "pw_hash TEXT 1  0", "created_at INTEGER 1  0",
			"ratings_version INTEGER 1 0 0",
		},
		"sessions": {
			"token_hash BLOB 0  1", "user_id INTEGER 1  0", "created_at INTEGER 1  0", "expires_at INTEGER 1  0",
		},
		"ratings": {
			"user_id INTEGER 1  1", "movie_id INTEGER 1  2", "rating REAL 1  0", "updated_at INTEGER 1  0",
		},
		"invites": {
			"code_hash BLOB 0  1", "max_uses INTEGER 1 1 0", "used_count INTEGER 1 0 0", "expires_at INTEGER 0  0",
			"note TEXT 0  0", "created_at INTEGER 1  0",
		},
		"feedback": {
			"user_id INTEGER 1  0", "movie_id INTEGER 1  0", "model TEXT 1  0", "action TEXT 1  0", "ts INTEGER 1  0",
		},
	}
	for table, cols := range want {
		got := columns(t, d.Reader, table)
		if strings.Join(got, "|") != strings.Join(cols, "|") {
			t.Errorf("%s columns:\n got %q\nwant %q", table, got, cols)
		}
	}
	// Spec constraints are in force.
	now := time.Now().Unix()
	if _, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', ?)", "ab", now); err == nil {
		t.Error("2-character username accepted")
	}
	if _, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', ?)", "Alice", now); err != nil {
		t.Fatal(err)
	}
	if _, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', ?)", "aLICE", now); err == nil {
		t.Error("usernames are not case-insensitively unique")
	}
	for _, r := range []float64{0, 5.5, 3.3} {
		if _, err := d.Writer.Exec("INSERT INTO ratings VALUES (1, 10, ?, ?)", r, now); err == nil {
			t.Errorf("rating %v accepted", r)
		}
	}
	if _, err := d.Writer.Exec("INSERT INTO sessions VALUES (x'00', 999, 0, 0)"); err == nil {
		t.Error("foreign key not enforced")
	}
	if _, err := d.Writer.Exec("INSERT INTO feedback VALUES (1, 1, 'pop', 'like', 0)"); err == nil {
		t.Error("feedback action check not enforced")
	}
	// A fresh database is not backed up.
	if _, err := os.Stat(filepath.Join(filepath.Dir(path), "backups")); !os.IsNotExist(err) {
		t.Fatalf("backups dir on a fresh DB: %v", err)
	}
}

func TestMigrateTwiceIsNoop(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	for i := 0; i < 2; i++ {
		if err := d.Migrate(ctx, quietLogger()); err != nil {
			t.Fatalf("run %d: %v", i, err)
		}
	}
	if v, _ := d.Version(ctx); v != latestVersion(t) {
		t.Fatalf("version = %d", v)
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(path), "backups")); !os.IsNotExist(err) {
		t.Fatal("a no-op migrate took a backup")
	}
}

func TestMigrateBacksUpNonEmptyDB(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	v1 := fstest.MapFS{"0001_a.sql": {Data: []byte("CREATE TABLE a (x INTEGER);")}}
	v2 := fstest.MapFS{
		"0001_a.sql": v1["0001_a.sql"],
		"0002_b.sql": {Data: []byte("CREATE TABLE b (y TEXT); INSERT INTO b VALUES ('two');")},
	}
	if err := d.migrate(ctx, quietLogger(), v1); err != nil {
		t.Fatal(err)
	}
	if _, err := d.Writer.Exec("INSERT INTO a VALUES (42)"); err != nil {
		t.Fatal(err)
	}
	var logs bytes.Buffer
	if err := d.migrate(ctx, slog.New(slog.NewTextHandler(&logs, nil)), v2); err != nil {
		t.Fatal(err)
	}
	if v, _ := d.Version(ctx); v != 2 {
		t.Fatalf("version = %d", v)
	}
	backups, _ := filepath.Glob(filepath.Join(filepath.Dir(path), "backups", "*.db"))
	if len(backups) != 1 {
		t.Fatalf("backups = %v", backups)
	}
	if !strings.Contains(logs.String(), "backup") {
		t.Errorf("backup not logged: %s", logs.String())
	}
	// Backups hold password hashes: owner-only.
	assertMode(t, filepath.Dir(backups[0]), 0o700)
	assertMode(t, backups[0], 0o600)
	// The backup is the pre-migration database.
	bk, err := Open(ctx, backups[0])
	if err != nil {
		t.Fatal(err)
	}
	defer bk.Close()
	var x int
	if err := bk.Reader.QueryRow("SELECT x FROM a").Scan(&x); err != nil || x != 42 {
		t.Fatalf("backup row = %d, %v", x, err)
	}
	if v, _ := bk.Version(ctx); v != 1 {
		t.Fatalf("backup version = %d", v)
	}
	if _, err := bk.Reader.Exec("SELECT * FROM b"); err == nil {
		t.Fatal("backup already has table b")
	}
}

func assertMode(t *testing.T, path string, want os.FileMode) {
	t.Helper()
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := st.Mode().Perm(); got != want {
		t.Errorf("%s mode %o, want %o", filepath.Base(path), got, want)
	}
}

// A pre-existing, too-open backups directory is tightened too.
func TestBackupDirTightened(t *testing.T) {
	d, path := openTemp(t)
	ctx := context.Background()
	dir := filepath.Join(filepath.Dir(path), "backups")
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := d.migrate(ctx, quietLogger(), fstest.MapFS{"0001_a.sql": {Data: []byte("CREATE TABLE a (x INTEGER);")}}); err != nil {
		t.Fatal(err)
	}
	if _, err := d.Writer.Exec("INSERT INTO a VALUES (1)"); err != nil {
		t.Fatal(err)
	}
	if _, err := d.backup(ctx, 1); err != nil {
		t.Fatal(err)
	}
	assertMode(t, dir, 0o700)
}

// Open creates a new app.db readable by the owner only.
func TestNewDatabaseFileIsOwnerOnly(t *testing.T) {
	d, path := openTemp(t)
	if _, err := d.Writer.Exec("CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (1);"); err != nil {
		t.Fatal(err)
	}
	assertMode(t, path, 0o600)
	if _, err := os.Stat(path + "-wal"); err == nil {
		assertMode(t, path+"-wal", 0o600)
	}
	// An existing file's mode is left as the operator set it.
	other := filepath.Join(t.TempDir(), "existing.db")
	if err := os.WriteFile(other, nil, 0o640); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(other, 0o640); err != nil {
		t.Fatal(err)
	}
	d2, err := Open(context.Background(), other)
	if err != nil {
		t.Fatal(err)
	}
	_ = d2.Close()
	assertMode(t, other, 0o640)
}

func TestMigrateAllOrNothing(t *testing.T) {
	d, _ := openTemp(t)
	ctx := context.Background()
	bad := fstest.MapFS{
		"0001_a.sql": {Data: []byte("CREATE TABLE a (x INTEGER);")},
		"0002_b.sql": {Data: []byte("CREATE TABLE b (y TEXT); THIS IS NOT SQL;")},
	}
	if err := d.migrate(ctx, quietLogger(), bad); err == nil {
		t.Fatal("broken migration applied")
	}
	if v, _ := d.Version(ctx); v != 0 {
		t.Fatalf("version = %d after failed migrate", v)
	}
	var n int
	if err := d.Reader.QueryRow("SELECT count(*) FROM sqlite_master").Scan(&n); err != nil || n != 0 {
		t.Fatalf("tables left behind: %d, %v", n, err)
	}
}

func TestMigrationFileNames(t *testing.T) {
	d, _ := openTemp(t)
	ctx := context.Background()
	for name, fsys := range map[string]fstest.MapFS{
		"gap":       {"0001_a.sql": {Data: []byte("SELECT 1;")}, "0003_c.sql": {Data: []byte("SELECT 1;")}},
		"bad name":  {"1_a.sql": {Data: []byte("SELECT 1;")}},
		"duplicate": {"0001_a.sql": {Data: []byte("SELECT 1;")}, "0001_b.sql": {Data: []byte("SELECT 1;")}},
		"no zero":   {"0000_a.sql": {Data: []byte("SELECT 1;")}},
	} {
		if err := d.migrate(ctx, quietLogger(), fsys); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

// After a rollback the older binary meets a newer schema; it must keep
// running (migrations are additive) and must not touch the database.
func TestNewerDatabaseVersionIsLeftAlone(t *testing.T) {
	d, _ := openTemp(t)
	ctx := context.Background()
	if _, err := d.Writer.Exec("PRAGMA user_version = 7"); err != nil {
		t.Fatal(err)
	}
	var logs bytes.Buffer
	if err := d.Migrate(ctx, slog.New(slog.NewTextHandler(&logs, nil))); err != nil {
		t.Fatal(err)
	}
	if v, _ := d.Version(ctx); v != 7 {
		t.Fatalf("version = %d", v)
	}
	if !strings.Contains(logs.String(), "newer") {
		t.Errorf("no warning logged: %s", logs.String())
	}
}

func TestEmbeddedMigrationsAreWellFormed(t *testing.T) {
	ms, err := loadMigrations(embedded())
	if err != nil {
		t.Fatal(err)
	}
	if len(ms) == 0 || ms[0].version != 1 {
		t.Fatalf("migrations = %+v", ms)
	}
}
