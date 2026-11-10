package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// `recs-api export-ratings` end to end: migrate, export, LATEST written.
func TestExportRatingsCommand(t *testing.T) {
	dir := t.TempDir()
	e := env(map[string]string{
		"RECS_DB_PATH":     filepath.Join(dir, "app.db"),
		"RECS_EXPORTS_DIR": filepath.Join(dir, "exports"),
	})
	var out, errb bytes.Buffer
	if code := run(context.Background(), []string{"migrate"}, e, &out, &errb, nil); code != 0 {
		t.Fatalf("migrate: %d %s", code, errb.String())
	}
	if code := run(context.Background(), []string{"export-ratings"}, e, &out, &errb, nil); code != 0 {
		t.Fatalf("export-ratings: %d %s", code, errb.String())
	}
	latest, err := os.ReadFile(filepath.Join(dir, "exports", "LATEST"))
	if err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(filepath.Join(dir, "exports", string(latest)))
	if err != nil || string(body) != "site_user_id,movie_id,rating,timestamp\n" {
		t.Fatalf("export %q: %q %v", latest, body, err)
	}
	if code := run(context.Background(), []string{"export-ratings", "--nope"}, e, &out, &errb, nil); code != 1 ||
		!strings.Contains(errb.String(), "usage: export-ratings") {
		t.Fatalf("bad flag: %d %s", code, errb.String())
	}
}
