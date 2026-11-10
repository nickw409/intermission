// Package migrations embeds the numbered SQLite migrations for app.db.
// internal/db applies them; see README.md for the rules.
package migrations

import "embed"

// FS holds every NNNN_name.sql file in this directory.
//
//go:embed *.sql
var FS embed.FS
