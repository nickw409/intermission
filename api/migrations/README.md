# migrations

Owner: workstream I1.

Numbered SQLite migrations for `app.db`, applied by `internal/db` at startup (and by
`recs-api migrate`).

- One file per migration: `NNNN_name.sql` (four-digit, zero-padded, contiguous from `0001`,
  e.g. `0001_init.sql`, `0002_ratings_version.sql`).
- Files are embedded into the binary with `embed.FS` from a `migrations.go` in this directory
  (`package migrations`, `//go:embed *.sql`).
- The applied version lives in `PRAGMA user_version`. Each pending file runs in its own
  transaction together with the `user_version` bump. A backup of `app.db` is taken before
  the first pending migration runs.
- Migrations only move forward and must work with both the old and the new recs-api
  (add columns; never rename or drop in the same release). Never edit a migration that has
  shipped; add a new one.
