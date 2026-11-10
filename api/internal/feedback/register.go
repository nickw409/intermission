package feedback

import (
	"net/http"

	"github.com/nickw409/intermission/api/internal/app"
)

// Register adds this package's routes to mux. Called once from main.go.
//
// TODO(phase 6 (workstream Q)): register routes.
func Register(mux *http.ServeMux, d *app.Deps) {}
