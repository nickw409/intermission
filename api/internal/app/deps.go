package app

import (
	"errors"
	"log/slog"

	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
)

// ErrNotImplemented is returned by stubs that a later workstream fills in.
// main prints "not implemented yet" and exits 1 when a subcommand returns it.
var ErrNotImplemented = errors.New("not implemented yet")

// Deps is everything a route-owning package needs. main builds exactly one
// and passes it to every Register(mux, deps) call.
type Deps struct {
	Config Config
	Logger *slog.Logger
	// DB holds the SQLite writer and reader pools (workstream I1).
	DB *db.DB
	// Model is the recs-model client (real HTTP client in production, the
	// modelclient/fake replay fake in tests).
	Model modelclient.Client
	// Movies is recs-api's SQLite movie metadata cache (workstream J), used
	// so ratings keep working while recs-model is down.
	Movies *moviecache.Cache
}
