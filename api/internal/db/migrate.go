package db

import (
	"context"
	"database/sql/driver"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"time"

	"github.com/nickw409/intermission/api/migrations"
)

// Migrations runner.
//
//   - Files are NNNN_name.sql, numbered contiguously from 0001, embedded from
//     api/migrations.
//   - PRAGMA user_version holds the number of the last applied file.
//   - Before anything is applied to a non-empty database, a consistent copy is
//     written with VACUUM INTO to <db dir>/backups/; old ones are pruned by
//     count and age (pruneBackups).
//   - All pending files and the user_version bump run in ONE transaction, so a
//     failure leaves the database exactly as it was. Foreign key enforcement
//     is off during that transaction (so a table can be rebuilt) and
//     foreign_key_check must pass before it commits.
//   - A database newer than this binary (after a rollback) is left untouched:
//     migrations are additive, so the older binary still works with it.

type migration struct {
	version int
	name    string
	sql     string
}

var migrationName = regexp.MustCompile(`^(\d{4})_[a-z0-9_]+\.sql$`)

func embedded() fs.FS { return migrations.FS }

// loadMigrations reads and validates every *.sql file in fsys.
func loadMigrations(fsys fs.FS) ([]migration, error) {
	entries, err := fs.ReadDir(fsys, ".")
	if err != nil {
		return nil, fmt.Errorf("read migrations: %w", err)
	}
	var ms []migration
	for _, e := range entries {
		if e.IsDir() || filepath.Ext(e.Name()) != ".sql" {
			continue
		}
		m := migrationName.FindStringSubmatch(e.Name())
		if m == nil {
			return nil, fmt.Errorf("migration %q: name must be NNNN_name.sql", e.Name())
		}
		v, _ := strconv.Atoi(m[1])
		body, err := fs.ReadFile(fsys, e.Name())
		if err != nil {
			return nil, err
		}
		ms = append(ms, migration{version: v, name: e.Name(), sql: string(body)})
	}
	sort.Slice(ms, func(i, j int) bool { return ms[i].version < ms[j].version })
	for i, m := range ms {
		if m.version != i+1 {
			return nil, fmt.Errorf("migration %q: expected number %04d (contiguous from 0001, no duplicates)", m.name, i+1)
		}
	}
	return ms, nil
}

func (d *DB) migrate(ctx context.Context, logger *slog.Logger, fsys fs.FS) error {
	ms, err := loadMigrations(fsys)
	if err != nil {
		return err
	}
	current, err := d.Version(ctx)
	if err != nil {
		return fmt.Errorf("read user_version: %w", err)
	}
	latest := len(ms)
	switch {
	case current > latest:
		logger.Warn("database schema is newer than this binary; leaving it alone",
			"db_version", current, "binary_version", latest)
		return nil
	case current == latest:
		logger.Debug("database schema up to date", "version", current)
		return nil
	}

	empty, err := d.isEmpty(ctx)
	if err != nil {
		return err
	}
	if !empty {
		path, err := d.backup(ctx, current)
		if err != nil {
			return fmt.Errorf("backup before migrating: %w", err)
		}
		logger.Info("database backup written before migrating", "backup", path, "version", current)
		if err := pruneBackups(filepath.Dir(path), time.Now()); err != nil {
			// The new backup exists; failing to delete old ones is not a
			// reason to refuse to start.
			logger.Warn("pruning old pre-migration backups failed", "err", err)
		}
	}

	if err := d.applyWithoutForeignKeys(ctx, ms[current:], latest); err != nil {
		return err
	}
	logger.Info("database migrated", "from", current, "to", latest)
	return nil
}

// applyWithoutForeignKeys runs the pending migrations and the user_version
// bump in one transaction with foreign key enforcement switched off, the
// procedure SQLite documents for rebuilding a table
// (https://www.sqlite.org/lang_altertable.html#otheralter): with foreign keys
// on, DROP TABLE on a parent table would first DELETE every row and so fire
// ON DELETE CASCADE on its children. PRAGMA foreign_keys is a no-op inside a
// transaction, so it is switched on the (single) writer connection, pinned
// for the duration, around the transaction. Before committing,
// foreign_key_check must come back empty, so a migration can never leave a
// dangling reference behind.
func (d *DB) applyWithoutForeignKeys(ctx context.Context, pending []migration, latest int) (err error) {
	conn, err := d.Writer.Conn(ctx)
	if err != nil {
		return err
	}
	defer func() {
		// Always switch enforcement back on before the connection returns to
		// the pool. If that fails, the connection must not be reused.
		if _, onErr := conn.ExecContext(context.WithoutCancel(ctx), "PRAGMA foreign_keys = ON"); onErr != nil {
			_ = conn.Raw(func(any) error { return driver.ErrBadConn })
			if err == nil {
				err = fmt.Errorf("re-enable foreign keys: %w", onErr)
			}
		}
		_ = conn.Close()
	}()
	if _, err := conn.ExecContext(ctx, "PRAGMA foreign_keys = OFF"); err != nil {
		return fmt.Errorf("disable foreign keys: %w", err)
	}

	tx, err := conn.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	for _, m := range pending {
		if _, err := tx.ExecContext(ctx, m.sql); err != nil {
			return fmt.Errorf("migration %s: %w", m.name, err)
		}
	}
	var violations int
	if err := tx.QueryRowContext(ctx, "SELECT count(*) FROM pragma_foreign_key_check").Scan(&violations); err != nil {
		return fmt.Errorf("foreign_key_check: %w", err)
	}
	if violations > 0 {
		return fmt.Errorf("migrations leave %d foreign key violations; nothing applied", violations)
	}
	// PRAGMA takes no bound parameters; latest is the count of embedded files.
	if _, err := tx.ExecContext(ctx, "PRAGMA user_version = "+strconv.Itoa(latest)); err != nil { // sqlguard:allow integer computed from embedded file names
		return fmt.Errorf("set user_version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit migrations: %w", err)
	}
	return nil
}

// Retention of pre-migration backups. Each one holds every account's
// password hash and ratings, including accounts deleted since, and the site
// promises that backups holding a deleted account's data are gone within 5
// weeks. So pruneBackups deletes a backup that is beyond the newest
// keepBackups OR older than maxBackupAge, except that a backup younger than
// minBackupAge is never deleted (a fresh migration always leaves a rollback
// point).
const (
	keepBackups  = 3
	maxBackupAge = 35 * 24 * time.Hour
	minBackupAge = 24 * time.Hour
)

// backupStampLayout is the timestamp format in backup file names (UTC); it
// sorts chronologically as text.
const backupStampLayout = "20060102T150405.000000000Z"

// backupName matches the files backup writes; the capture is the timestamp.
var backupName = regexp.MustCompile(`^app-pre-migrate-v\d+-(\d{8}T\d{6}\.\d{9}Z)\.db$`)

// pruneBackups applies the retention rules above to the pre-migration
// backups in dir, as of now. Files that don't match backupName (or whose
// timestamp doesn't parse) are never touched.
func pruneBackups(dir string, now time.Time) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	type backupFile struct {
		name string
		at   time.Time
	}
	var found []backupFile
	for _, e := range entries {
		m := backupName.FindStringSubmatch(e.Name())
		if m == nil || !e.Type().IsRegular() {
			continue
		}
		at, err := time.Parse(backupStampLayout, m[1])
		if err != nil {
			continue
		}
		found = append(found, backupFile{e.Name(), at})
	}
	sort.Slice(found, func(i, j int) bool { return found[i].at.After(found[j].at) }) // newest first
	var errs []error
	for i, b := range found {
		age := now.Sub(b.at)
		if age < minBackupAge || (i < keepBackups && age <= maxBackupAge) {
			continue
		}
		if err := os.Remove(filepath.Join(dir, b.name)); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func (d *DB) isEmpty(ctx context.Context) (bool, error) {
	var n int
	err := d.Writer.QueryRowContext(ctx, "SELECT count(*) FROM sqlite_master").Scan(&n)
	return n == 0, err
}

// backup writes a consistent copy of the database with VACUUM INTO and
// returns its path. Backups contain password hashes, so the directory is
// forced to 0700 and the file is created 0600 before SQLite writes to it
// (VACUUM INTO accepts an existing empty file), then re-checked as 0600.
func (d *DB) backup(ctx context.Context, version int) (string, error) {
	dir := filepath.Join(filepath.Dir(d.path), "backups")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", err
	}
	if err := os.Chmod(dir, 0o700); err != nil {
		return "", err
	}
	stamp := time.Now().UTC().Format("20060102T150405.000000000Z")
	path := filepath.Join(dir, "app-pre-migrate-v"+strconv.Itoa(version)+"-"+stamp+".db")
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", err
	}
	if err := f.Close(); err != nil {
		return "", err
	}
	if _, err := d.Writer.ExecContext(ctx, "VACUUM INTO ?", path); err != nil {
		_ = os.Remove(path)
		return "", err
	}
	if err := os.Chmod(path, 0o600); err != nil {
		return "", err
	}
	return path, nil
}
