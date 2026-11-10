# migrations

Owner: workstream I1.

Numbered SQLite migrations for `app.db`, applied by `internal/db` at startup (and by
`recs-api migrate`).

- One file per migration: `NNNN_name.sql` (four-digit, zero-padded, contiguous from `0001`,
  e.g. `0001_init.sql`, `0002_ratings_version.sql`).
- Files are embedded into the binary with `embed.FS` from a `migrations.go` in this directory
  (`package migrations`, `//go:embed *.sql`).
- The applied version lives in `PRAGMA user_version`. All pending files run in ONE
  transaction together with the `user_version` bump, so a failure leaves the database
  exactly as it was. Before migrating a non-empty database, a consistent copy is written
  with `VACUUM INTO` to `<db dir>/backups/app-pre-migrate-v<N>-<UTC timestamp>.db`;
  they hold deleted accounts' data, so one is deleted once it is beyond the newest 3 or
  older than 35 days, but never while it is younger than 1 day.
- Foreign key enforcement is OFF while the migration transaction runs (SQLite's
  documented table-rebuild procedure needs it: with it on, `DROP TABLE users` would
  cascade into every child table). `PRAGMA foreign_key_check` must come back empty
  before the transaction commits, so a migration cannot leave dangling references.
  Running `migrate` on an up-to-date database is a no-op (no backup).
- A database whose `user_version` is newer than the binary (after a rollback) is left
  untouched with a warning.
- Migrations only move forward and must work with both the old and the new recs-api
  (add columns; never rename or drop in the same release). Never edit a migration that has
  shipped; add a new one.
