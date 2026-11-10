package main

import (
	"context"

	"github.com/nickw409/intermission/api/internal/app"
)

// runAdmin handles `recs-api admin ...`, e.g.
// `recs-api admin invite create --uses N --expires 14d --note "name"`, which
// prints the new code once. args are the arguments after "admin".
//
// Owner: workstream I1 (api/cmd/recs-api/admin*.go).
// TODO(workstream I1): implement.
func runAdmin(ctx context.Context, d *app.Deps, args []string) error {
	return app.ErrNotImplemented
}
