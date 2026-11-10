package main

import (
	"bytes"
	"context"
	"database/sql"
	"io"
	"log/slog"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/invites"
)

func adminDeps(t *testing.T) *app.Deps {
	t.Helper()
	database, err := db.Open(context.Background(), filepath.Join(t.TempDir(), "app.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = database.Close() })
	return &app.Deps{DB: database, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
}

var codeLine = regexp.MustCompile(`^[A-Z2-7]{4}(-[A-Z2-7]{4}){5}-[A-Z2-7]{2}\n$`)

func TestAdminInviteCreate(t *testing.T) {
	d := adminDeps(t)
	ctx := context.Background()
	var out bytes.Buffer
	before := time.Now().Unix()
	if err := runAdminTo(ctx, d, []string{"invite", "create", "--uses", "3", "--expires", "14d", "--note", "for bob"}, &out); err != nil {
		t.Fatal(err)
	}
	if !codeLine.MatchString(out.String()) {
		t.Fatalf("stdout = %q, want just the code", out.String())
	}
	code := strings.TrimSpace(out.String())
	var maxUses, used int
	var expires sql.NullInt64
	var note string
	err := d.DB.Reader.QueryRow("SELECT max_uses, used_count, expires_at, note FROM invites WHERE code_hash = ?", invites.Hash(code)).
		Scan(&maxUses, &used, &expires, &note)
	if err != nil {
		t.Fatal(err)
	}
	want := before + 14*24*3600
	if maxUses != 3 || used != 0 || note != "for bob" || expires.Int64 < want || expires.Int64 > want+5 {
		t.Fatalf("row: uses %d used %d expires %v note %q", maxUses, used, expires, note)
	}
	// The code works for signup immediately.
	if err := invites.New(d.DB).Check(ctx, code); err != nil {
		t.Fatal(err)
	}
}

func TestAdminInviteDefaultsAndDurations(t *testing.T) {
	d := adminDeps(t)
	ctx := context.Background()
	for _, c := range []struct {
		args    []string
		uses    int
		expires time.Duration // 0 = never
	}{
		{nil, 1, 14 * 24 * time.Hour},
		{[]string{"--expires", "never"}, 1, 0},
		{[]string{"--expires", "36h", "--uses", "2"}, 2, 36 * time.Hour},
		{[]string{"--expires=1d"}, 1, 24 * time.Hour},
	} {
		var out bytes.Buffer
		before := time.Now().Unix()
		if err := runAdminTo(ctx, d, append([]string{"invite", "create"}, c.args...), &out); err != nil {
			t.Fatalf("%v: %v", c.args, err)
		}
		var uses int
		var expires sql.NullInt64
		if err := d.DB.Reader.QueryRow("SELECT max_uses, expires_at FROM invites WHERE code_hash = ?",
			invites.Hash(strings.TrimSpace(out.String()))).Scan(&uses, &expires); err != nil {
			t.Fatal(err)
		}
		if uses != c.uses {
			t.Errorf("%v: uses %d", c.args, uses)
		}
		switch {
		case c.expires == 0 && expires.Valid:
			t.Errorf("%v: expires %v, want never", c.args, expires)
		case c.expires != 0 && (expires.Int64 < before+int64(c.expires.Seconds()) || expires.Int64 > before+int64(c.expires.Seconds())+5):
			t.Errorf("%v: expires %v", c.args, expires)
		}
	}
}

func TestAdminRejectsBadInput(t *testing.T) {
	d := adminDeps(t)
	for _, args := range [][]string{
		nil,
		{"invite"},
		{"invite", "delete"},
		{"user", "create"},
		{"invite", "create", "--uses", "0"},
		{"invite", "create", "--uses", "-3"},
		{"invite", "create", "--uses", "x"},
		{"invite", "create", "--expires", "0d"},
		{"invite", "create", "--expires", "-1h"},
		{"invite", "create", "--expires", "soon"},
		{"invite", "create", "--expires", "100000d"},
		{"invite", "create", "extra"},
		{"invite", "create", "--bogus"},
	} {
		var out bytes.Buffer
		if err := runAdminTo(context.Background(), d, args, &out); err == nil {
			t.Errorf("%v accepted", args)
		}
		if out.Len() != 0 {
			t.Errorf("%v printed %q", args, out.String())
		}
	}
}

func TestMigrateAndAdminThroughRun(t *testing.T) {
	dir := t.TempDir()
	e := env(map[string]string{"RECS_DB_PATH": filepath.Join(dir, "app.db")})
	var out, errb bytes.Buffer
	if code := run(context.Background(), []string{"migrate"}, e, &out, &errb, nil); code != 0 {
		t.Fatalf("migrate: %d %s", code, errb.String())
	}
	if code := run(context.Background(), []string{"migrate"}, e, &out, &errb, nil); code != 0 {
		t.Fatalf("second migrate: %d %s", code, errb.String())
	}
	if code := run(context.Background(), []string{"admin", "invite", "create", "--uses", "x"}, e, &out, &errb, nil); code != 1 {
		t.Fatalf("bad admin: %d", code)
	}
}
