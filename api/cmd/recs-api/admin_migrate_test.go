package main

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

// PR #42 review L6: `recs-api admin` runs as a second process next to a live
// server, so it must never migrate (rebuild tables) itself. On a schema that
// is behind, it refuses with a clear message and changes nothing.
func TestAdminRefusesToMigrate(t *testing.T) {
	d := adminDeps(t) // a fresh, un-migrated database
	var out bytes.Buffer
	err := runAdminTo(context.Background(), d, []string{"invite", "create"}, &out)
	if err == nil {
		t.Fatal("admin ran on an un-migrated database")
	}
	if !strings.Contains(err.Error(), "recs-api migrate") {
		t.Errorf("error does not say what to do: %v", err)
	}
	if out.Len() != 0 {
		t.Errorf("printed %q", out.String())
	}
	var tables int
	if err := d.DB.Reader.QueryRow("SELECT count(*) FROM sqlite_master").Scan(&tables); err != nil {
		t.Fatal(err)
	}
	if tables != 0 {
		t.Fatalf("admin created %d schema objects", tables)
	}
}
