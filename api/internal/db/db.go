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
	"log/slog"
)

// ErrNotImplemented marks stubs workstream I1 has not filled in yet.
var ErrNotImplemented = errors.New("not implemented yet")

// DB is the pair of pools on app.db.
type DB struct {
	Writer *sql.DB // SetMaxOpenConns(1)
	Reader *sql.DB
}

// Open opens app.db at path.
//
// TODO(workstream I1): open both pools. The stub returns a DB with nil pools
// so the skeleton server can start.
func Open(ctx context.Context, path string) (*DB, error) {
	return &DB{}, nil
}

// Migrate backs up the database and applies pending migrations.
//
// TODO(workstream I1): implement. The stub returns ErrNotImplemented.
func (d *DB) Migrate(ctx context.Context, logger *slog.Logger) error {
	return ErrNotImplemented
}

// Close closes both pools.
func (d *DB) Close() error {
	var errs []error
	if d.Writer != nil {
		errs = append(errs, d.Writer.Close())
	}
	if d.Reader != nil {
		errs = append(errs, d.Reader.Close())
	}
	return errors.Join(errs...)
}
