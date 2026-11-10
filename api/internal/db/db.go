// Package db opens app.db and applies migrations.
//
// Owner: workstream I1. Responsibilities: two database/sql pools on the same
// SQLite file (writer capped at one connection, reader pool), WAL mode,
// foreign_keys=ON, busy_timeout 5000 ms; the migrations runner (embed.FS
// from api/migrations, PRAGMA user_version, backup before migrating). Every
// SQL statement is prepared with bound parameters.
package db

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"net/url"
	"os"
	"runtime"
	"slices"
	"strings"
	"sync"

	_ "modernc.org/sqlite" // registers the "sqlite" database/sql driver (pure Go)
)

// ErrNotImplemented marks stubs workstream I1 has not filled in yet. Nothing
// returns it any more; it stays because main.go refers to it.
var ErrNotImplemented = errors.New("not implemented yet")

// DB is the pair of pools on app.db.
type DB struct {
	Writer *sql.DB // SetMaxOpenConns(1)
	Reader *sql.DB
	path   string

	hooksMu     sync.Mutex
	userDeleted []func(userID int64)

	// maint is a one-connection pool with a short busy timeout for
	// checkpoints (scrub.go), so a checkpoint waiting on a reader never
	// holds the writer connection or blocks writers for long.
	maint *sql.DB
	scrub scrubState
}

// BusyTimeoutMS is how long a connection waits on a lock before SQLITE_BUSY.
const BusyTimeoutMS = 5000

// Open opens app.db at path: a writer pool limited to one connection (so
// writes queue in Go instead of fighting over SQLite's lock) and a read-only
// reader pool. Both use WAL, foreign_keys=ON, secure_delete=ON,
// journal_size_limit=0 and busy_timeout=5000. Writer
// transactions start with BEGIN IMMEDIATE so a transaction that reads and
// then writes can never fail half-way on a lock upgrade.
//
// The parent directory must already exist. A new database file is created
// with mode 0600 (SQLite gives its -wal and -shm files the same mode); an
// existing file's mode is left alone.
func Open(ctx context.Context, path string) (*DB, error) {
	if path == "" || strings.ContainsAny(path, "?#") || strings.HasPrefix(path, "file:") || path == ":memory:" {
		return nil, fmt.Errorf("db: unsupported database path %q", path)
	}
	if err := createPrivate(path); err != nil {
		return nil, fmt.Errorf("db: create %s: %w", path, err)
	}
	common := []string{
		"busy_timeout(5000)",
		"foreign_keys(1)",
		"journal_mode(WAL)",
		"synchronous(NORMAL)",
		// Overwrite deleted content with zeros, including whole pages that
		// go to the freelist (FAST skips those, and a deleted account with
		// a few thousand ratings frees whole pages: security audit SA5-3).
		// The zeroed pages go through the WAL; the deleted page images
		// already in the WAL leave it at the next TRUNCATE checkpoint
		// (scrub.go).
		"secure_delete(1)",
		// Shrink the WAL file back to zero bytes whenever it is reset after
		// a checkpoint, instead of leaving old frames in a large file.
		"journal_size_limit(0)",
	}
	writer, err := openPool(ctx, path, common, "immediate")
	if err != nil {
		return nil, fmt.Errorf("db: open writer: %w", err)
	}
	writer.SetMaxOpenConns(1)
	writer.SetMaxIdleConns(1)
	writer.SetConnMaxLifetime(0)

	reader, err := openPool(ctx, path, append(common, "query_only(1)"), "deferred")
	if err != nil {
		_ = writer.Close()
		return nil, fmt.Errorf("db: open reader: %w", err)
	}
	n := max(4, runtime.NumCPU())
	reader.SetMaxOpenConns(n)
	reader.SetMaxIdleConns(n)

	maint, err := openPool(ctx, path, []string{"busy_timeout(" + maintBusyMS + ")", "journal_mode(WAL)"}, "deferred")
	if err != nil {
		_ = writer.Close()
		_ = reader.Close()
		return nil, fmt.Errorf("db: open maintenance pool: %w", err)
	}
	maint.SetMaxOpenConns(1)
	maint.SetMaxIdleConns(1)
	return &DB{Writer: writer, Reader: reader, maint: maint, path: path}, nil
}

// createPrivate creates an empty file at path with mode 0600 unless one exists.
func createPrivate(path string) error {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o600)
	if errors.Is(err, fs.ErrExist) {
		return nil
	}
	if err != nil {
		return err
	}
	return f.Close()
}

func openPool(ctx context.Context, path string, pragmas []string, txlock string) (*sql.DB, error) {
	q := url.Values{}
	for _, p := range pragmas {
		q.Add("_pragma", p)
	}
	q.Set("_txlock", txlock)
	pool, err := sql.Open("sqlite", "file:"+path+"?"+q.Encode())
	if err != nil {
		return nil, err
	}
	if err := pool.PingContext(ctx); err != nil {
		_ = pool.Close()
		return nil, err
	}
	return pool, nil
}

// Path returns the database file path.
func (d *DB) Path() string { return d.path }

// Version returns PRAGMA user_version, the number of the last applied migration.
func (d *DB) Version(ctx context.Context) (int, error) {
	var v int
	err := d.Writer.QueryRowContext(ctx, "PRAGMA user_version").Scan(&v)
	return v, err
}

// Migrate backs up the database and applies pending migrations; see migrate.go.
func (d *DB) Migrate(ctx context.Context, logger *slog.Logger) error {
	return d.migrate(ctx, logger, embedded())
}

// OnUserDeleted registers f to run after an account is deleted. Packages
// that keep per-user state in memory (the recommendation cache, per-user
// rate limits) use it to forget that user, so nothing they hold can outlive
// the account. f runs synchronously in the deleting request and must be
// quick and must not call back into OnUserDeleted.
//
// The registry lives here because every package already shares the one
// *DB, and the package that deletes accounts (auth) must not import the
// packages that cache per-user data (recommend imports auth).
func (d *DB) OnUserDeleted(f func(userID int64)) {
	d.hooksMu.Lock()
	defer d.hooksMu.Unlock()
	d.userDeleted = append(d.userDeleted, f)
}

// UserDeleted runs every OnUserDeleted hook. The caller has committed the
// deletion.
func (d *DB) UserDeleted(userID int64) {
	d.hooksMu.Lock()
	hooks := slices.Clone(d.userDeleted)
	d.hooksMu.Unlock()
	for _, f := range hooks {
		f(userID)
	}
}

// Close stops any pending background checkpoint and closes the pools.
func (d *DB) Close() error {
	d.stopScrub()
	var errs []error
	for _, p := range []*sql.DB{d.Writer, d.Reader, d.maint} {
		if p != nil {
			errs = append(errs, p.Close())
		}
	}
	return errors.Join(errs...)
}
