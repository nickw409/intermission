package main

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"testing"
)

// Security audit SA1-6: the invite note names a person; it is stored but
// never logged. The log records only whether one was set.
func TestAdminInviteNoteNotLogged(t *testing.T) {
	for _, c := range []struct {
		args []string
		want string
	}{
		{[]string{"--note", "Canary Person 7f3a"}, "note_set=true"},
		{nil, "note_set=false"},
	} {
		d := migratedAdminDeps(t)
		var logs bytes.Buffer
		d.Logger = slog.New(slog.NewTextHandler(&logs, nil))
		var out bytes.Buffer
		if err := runAdminTo(context.Background(), d, append([]string{"invite", "create"}, c.args...), &out); err != nil {
			t.Fatal(err)
		}
		if strings.Contains(logs.String(), "Canary") {
			t.Errorf("invite note logged: %s", logs.String())
		}
		if !strings.Contains(logs.String(), c.want) {
			t.Errorf("log lacks %s: %s", c.want, logs.String())
		}
	}
}
