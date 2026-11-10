// Command recs-api is Intermission's product API: accounts, sessions,
// invites, ratings, the recs-model client, posters and run reports.
//
// WIRING FILE (owner: orchestrator). Workstreams never edit it: every package
// is already registered below, and each package fills in its own Register,
// Run or stub function.
//
// Usage:
//
//	recs-api serve
//	recs-api migrate
//	recs-api export-ratings [flags]
//	recs-api admin invite create --uses N --expires 14d --note "name"
//
// Configuration comes from the environment; see internal/app/config.go.
package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/export"
	"github.com/nickw409/intermission/api/internal/feedback"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/movies"
	"github.com/nickw409/intermission/api/internal/posters"
	"github.com/nickw409/intermission/api/internal/ratings"
	"github.com/nickw409/intermission/api/internal/recommend"
	"github.com/nickw409/intermission/api/internal/reports"
)

// registrars lists every route-owning package. Each adds its own routes.
var registrars = []func(*http.ServeMux, *app.Deps){
	httpx.Register,     // I1: /healthz
	auth.Register,      // I1: /api/auth/*, /api/me
	ratings.Register,   // J: /api/ratings
	movies.Register,    // J: /api/movies/*
	recommend.Register, // J: /api/recommend, /api/recommend/compare
	reports.Register,   // J: /api/reports
	posters.Register,   // J: /api/posters/{movieId}
	feedback.Register,  // phase 6: /api/feedback
}

const usage = `usage: recs-api <command> [args]

commands:
  serve                                 run the HTTP API
  migrate                               apply database migrations and exit
  export-ratings [flags]                write website ratings to RECS_EXPORTS_DIR
  admin invite create --uses N --expires 14d --note "name"
                                        mint an invite code (printed once)
`

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := run(ctx, os.Args[1:], os.Getenv, os.Stdout, os.Stderr, nil)
	stop()
	os.Exit(code)
}

// run executes one subcommand and returns the process exit code. ready, if
// non-nil, is called with the listen address once serve is accepting.
func run(ctx context.Context, args []string, getenv func(string) string, stdout, stderr io.Writer, ready func(addr string)) int {
	if len(args) == 0 {
		fmt.Fprint(stderr, usage)
		return 2
	}
	switch args[0] {
	case "help", "-h", "--help":
		fmt.Fprint(stdout, usage)
		return 0
	case "serve", "migrate", "export-ratings", "admin":
	default:
		fmt.Fprintf(stderr, "unknown command %q\n\n%s", args[0], usage)
		return 2
	}

	cfg, err := app.LoadConfig(getenv)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	logger := slog.New(slog.NewJSONHandler(stderr, &slog.HandlerOptions{Level: cfg.LogLevel}))
	slog.SetDefault(logger)

	d, err := newDeps(ctx, cfg, logger)
	if err != nil {
		logger.Error("startup failed", "err", err)
		return 1
	}
	defer func() {
		if err := d.DB.Close(); err != nil {
			logger.Error("closing database", "err", err)
		}
	}()

	switch args[0] {
	case "serve":
		err = serve(ctx, d, ready)
	case "migrate":
		err = d.DB.Migrate(ctx, logger)
	case "export-ratings":
		err = export.Run(ctx, d, args[1:])
	case "admin":
		err = runAdmin(ctx, d, args[1:])
	}
	switch {
	case isNotImplemented(err):
		fmt.Fprintf(stderr, "%s: not implemented yet\n", args[0])
		return 1
	case err != nil:
		logger.Error("command failed", "command", args[0], "err", err)
		return 1
	}
	return 0
}

func isNotImplemented(err error) bool {
	return errors.Is(err, app.ErrNotImplemented) || errors.Is(err, db.ErrNotImplemented)
}

// newDeps builds the shared dependencies.
func newDeps(ctx context.Context, cfg app.Config, logger *slog.Logger) (*app.Deps, error) {
	database, err := db.Open(ctx, cfg.DBPath)
	if err != nil {
		return nil, fmt.Errorf("open database: %w", err)
	}
	model := modelclient.NewHTTP(modelclient.HTTPConfig{
		SocketPath: cfg.ModelSocket,
		Addr:       cfg.ModelAddr,
		Timeout:    modelclient.DefaultTimeout,
		Logger:     logger,
	})
	return &app.Deps{
		Config: cfg,
		Logger: logger,
		DB:     database,
		Model:  model,
		Movies: moviecache.New(database, model),
	}, nil
}

// serve migrates the database, registers every package's routes and serves
// HTTP until ctx is cancelled (SIGINT/SIGTERM), then shuts down gracefully.
func serve(ctx context.Context, d *app.Deps, ready func(addr string)) error {
	logger := d.Logger
	if err := d.DB.Migrate(ctx, logger); isNotImplemented(err) {
		logger.Warn("database migrations not implemented yet; skipping")
	} else if err != nil {
		return fmt.Errorf("migrate: %w", err)
	}

	mux := http.NewServeMux()
	for _, register := range registrars {
		register(mux, d)
	}

	srv := &http.Server{
		Handler:           httpx.Wrap(mux, d),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    16 << 10,
		ErrorLog:          slog.NewLogLogger(logger.Handler(), slog.LevelWarn),
		BaseContext:       func(net.Listener) context.Context { return context.WithoutCancel(ctx) },
	}
	ln, err := net.Listen("tcp", d.Config.Addr)
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	logger.Info("recs-api listening", "addr", ln.Addr().String(), "config", d.Config)

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(ln) }()
	if ready != nil {
		ready(ln.Addr().String())
	}

	select {
	case err := <-errc:
		return fmt.Errorf("serve: %w", err)
	case <-ctx.Done():
	}
	logger.Info("shutting down")
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("shutdown: %w", err)
	}
	return nil
}
