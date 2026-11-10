package export

import (
	"context"

	"github.com/nickw409/intermission/api/internal/app"
)

// Run executes `recs-api export-ratings [flags]`. args are the arguments
// after the subcommand name.
//
// TODO(workstream J): implement.
func Run(ctx context.Context, d *app.Deps, args []string) error {
	return app.ErrNotImplemented
}
