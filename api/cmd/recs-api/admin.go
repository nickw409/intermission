package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/invites"
)

// Owner: workstream I1 (api/cmd/recs-api/admin*.go).

const adminUsage = `usage: recs-api admin invite create [--uses N] [--expires 14d|36h|never] [--note "name"]`

// maxInviteLifetime bounds --expires.
const maxInviteLifetime = 365 * 24 * time.Hour

// runAdmin handles `recs-api admin ...`, e.g.
// `recs-api admin invite create --uses N --expires 14d --note "name"`, which
// prints the new code once. args are the arguments after "admin".
func runAdmin(ctx context.Context, d *app.Deps, args []string) error {
	return runAdminTo(ctx, d, args, os.Stdout)
}

// runAdminTo is runAdmin writing the code to out. Only the code goes to out
// (one line), so it can be piped; it is never logged.
func runAdminTo(ctx context.Context, d *app.Deps, args []string, out io.Writer) error {
	if len(args) < 2 || args[0] != "invite" || args[1] != "create" {
		return errors.New(adminUsage)
	}
	fs := flag.NewFlagSet("admin invite create", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	uses := fs.Int("uses", 1, "how many signups the code allows")
	expires := fs.String("expires", "14d", "lifetime: Nd (days), a Go duration such as 36h, or never")
	note := fs.String("note", "", "who the code is for")
	if err := fs.Parse(args[2:]); err != nil {
		return fmt.Errorf("%w\n%s", err, adminUsage)
	}
	if fs.NArg() != 0 {
		return fmt.Errorf("unexpected arguments %q\n%s", fs.Args(), adminUsage)
	}
	if *uses < 1 || *uses > invites.MaxUses {
		return fmt.Errorf("--uses must be between 1 and %d", invites.MaxUses)
	}
	lifetime, err := parseLifetime(*expires)
	if err != nil {
		return err
	}

	// The command may run before the server ever has; make sure the schema exists.
	if err := d.DB.Migrate(ctx, d.Logger); err != nil {
		return fmt.Errorf("migrate: %w", err)
	}
	code, err := invites.New(d.DB).Create(ctx, *uses, lifetime, *note)
	if err != nil {
		return err
	}
	// The note names a person (personal data): log only whether one was set.
	d.Logger.Info("invite created", "uses", *uses, "expires_in", lifetime.String(), "note_set", *note != "")
	_, err = fmt.Fprintln(out, code)
	return err
}

// parseLifetime parses --expires: "never" (0), "Nd" days, or a Go duration.
func parseLifetime(s string) (time.Duration, error) {
	if s == "never" {
		return 0, nil
	}
	var d time.Duration
	if days, ok := strings.CutSuffix(s, "d"); ok {
		n, err := strconv.Atoi(days)
		if err != nil {
			return 0, fmt.Errorf("--expires %q: want Nd, a duration like 36h, or never", s)
		}
		d = time.Duration(n) * 24 * time.Hour
		if n > int(maxInviteLifetime/(24*time.Hour)) {
			d = maxInviteLifetime + 1
		}
	} else {
		var err error
		if d, err = time.ParseDuration(s); err != nil {
			return 0, fmt.Errorf("--expires %q: want Nd, a duration like 36h, or never", s)
		}
	}
	if d <= 0 || d > maxInviteLifetime {
		return 0, fmt.Errorf("--expires must be positive and at most %s (or never)", "365d")
	}
	return d, nil
}
