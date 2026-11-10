package auth

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// Register adds this package's routes to mux. Called once from main.go.
//
//	POST   /api/auth/signup
//	POST   /api/auth/login
//	POST   /api/auth/logout   (session)
//	GET    /api/me            (session)
//	DELETE /api/me            (session)
//
// CSRF checks and the body cap are applied to every mutating route by
// httpx.Wrap.
func Register(mux *http.ServeMux, d *app.Deps) {
	s, err := newService(d, options{
		Hasher:          NewArgon2id(DefaultParams, MaxConcurrentHashes),
		Now:             time.Now,
		LoginPerAccount: ratelimit.LoginPerAccount,
		LoginGlobal:     ratelimit.LoginGlobal,
		SignupGlobal:    ratelimit.SignupGlobal,
	})
	if err != nil {
		// Only crypto/rand failing can get here; the process can't run safely.
		panic(fmt.Sprintf("auth: %v", err))
	}
	s.register(mux)
}

// options are the service's swappable parts (tests use a counting hasher, a
// fake clock and different limits).
type options struct {
	Hasher          Hasher
	Now             func() time.Time
	LoginPerAccount ratelimit.Config
	LoginGlobal     ratelimit.Config
	SignupGlobal    ratelimit.Config
}

type service struct {
	db       *db.DB
	logger   *slog.Logger
	mode     app.SignupMode
	hasher   Hasher
	now      func() time.Time
	invites  *invites.Store
	sessions *sessions

	loginPerAccount *ratelimit.Keyed
	loginGlobal     *ratelimit.Global
	signupGlobal    *ratelimit.Global

	// dummyHash is verified against when a login names an unknown user, so
	// that path costs the same single Argon2id run as a wrong password.
	dummyHash string
}

func newService(d *app.Deps, o options) (*service, error) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	clock := ratelimit.WithClock(o.Now)
	s := &service{
		db:              d.DB,
		logger:          logger,
		mode:            d.Config.SignupMode,
		hasher:          o.Hasher,
		now:             o.Now,
		invites:         invites.NewWithClock(d.DB, o.Now),
		sessions:        &sessions{db: d.DB, now: o.Now, logger: logger},
		loginPerAccount: ratelimit.NewKeyed(o.LoginPerAccount, clock),
		loginGlobal:     ratelimit.NewGlobal(o.LoginGlobal, clock),
		signupGlobal:    ratelimit.NewGlobal(o.SignupGlobal, clock),
	}
	// A random password nobody knows, hashed with the same parameters as real
	// accounts.
	var pw [24]byte
	if _, err := rand.Read(pw[:]); err != nil {
		return nil, err
	}
	dummy, err := o.Hasher.Hash(context.Background(), base64.RawStdEncoding.EncodeToString(pw[:]))
	if err != nil {
		return nil, err
	}
	s.dummyHash = dummy
	return s, nil
}

func (s *service) register(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/auth/signup", s.signup)
	mux.HandleFunc("POST /api/auth/login", s.login)
	mux.Handle("POST /api/auth/logout", s.sessions.require(http.HandlerFunc(s.logout)))
	mux.Handle("GET /api/me", s.sessions.require(http.HandlerFunc(s.getMe)))
	mux.Handle("DELETE /api/me", s.sessions.require(http.HandlerFunc(s.deleteMe)))
}
