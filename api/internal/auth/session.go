package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"log/slog"
	"net/http"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Sessions (spec: "Sessions").
//
// A session token is 32 bytes from crypto/rand, base64url-encoded (43
// characters), sent only in the `session` cookie (HttpOnly; Secure;
// SameSite=Lax; Path=/; Max-Age 30 days). The database stores only
// SHA-256(token), so a leaked database can't be replayed as cookies. A plain
// hash is right here (no salt or slow KDF): the token has 256 random bits.
// Sessions expire 30 days after creation, are replaced on every login and
// deleted on logout.

// CookieName is the session cookie's name (contracts/public.yaml).
const CookieName = "session"

// SessionTTL is how long a session lives.
const SessionTTL = 30 * 24 * time.Hour

const tokenBytes = 32

var tokenEncoding = base64.RawURLEncoding

var errNoSession = apperr.New(http.StatusUnauthorized, apperr.CodeUnauthorized, "not logged in")

// newToken returns a fresh token and the hash to store.
func newToken() (string, []byte, error) {
	var raw [tokenBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", nil, err
	}
	tok := tokenEncoding.EncodeToString(raw[:])
	return tok, hashToken(tok), nil
}

func hashToken(tok string) []byte {
	sum := sha256.Sum256([]byte(tok))
	return sum[:]
}

// tokenFromRequest returns the hash of the request's session token, if the
// cookie is present and well formed. Malformed values never reach the DB.
func tokenFromRequest(r *http.Request) ([]byte, bool) {
	c, err := r.Cookie(CookieName)
	if err != nil || len(c.Value) != tokenEncoding.EncodedLen(tokenBytes) {
		return nil, false
	}
	raw, err := tokenEncoding.Strict().DecodeString(c.Value)
	if err != nil || len(raw) != tokenBytes {
		return nil, false
	}
	return hashToken(c.Value), true
}

func sessionCookie(token string) *http.Cookie {
	return &http.Cookie{
		Name:     CookieName,
		Value:    token,
		Path:     "/",
		MaxAge:   int(SessionTTL / time.Second),
		HttpOnly: true,
		Secure:   true,
		SameSite: http.SameSiteLaxMode,
	}
}

func clearedCookie() *http.Cookie {
	c := sessionCookie("")
	c.MaxAge = -1 // Max-Age=0
	return c
}

// Session is the authenticated session attached to a request context.
type Session struct {
	UserID    int64
	tokenHash []byte
}

type sessionKey struct{}

// UserID returns the authenticated user's ID from a context set up by
// RequireSession.
func UserID(ctx context.Context) (int64, bool) {
	s, ok := ctx.Value(sessionKey{}).(*Session)
	if !ok {
		return 0, false
	}
	return s.UserID, true
}

// SessionKey identifies the request's session for per-session rate limiting
// (ratelimit.Middleware with ratelimit.RecommendPerSession): the hex SHA-256
// of the token, never the token itself.
func SessionKey(r *http.Request) (string, bool) {
	s, ok := r.Context().Value(sessionKey{}).(*Session)
	if !ok {
		return "", false
	}
	return hex.EncodeToString(s.tokenHash), true
}

// sessions reads and writes the sessions table.
type sessions struct {
	db     *db.DB
	now    func() time.Time
	logger *slog.Logger
}

// lookup returns the live session for tokenHash, or errNoSession.
func (s *sessions) lookup(ctx context.Context, tokenHash []byte) (*Session, error) {
	var uid int64
	err := s.db.Reader.QueryRowContext(ctx,
		"SELECT user_id FROM sessions WHERE token_hash = ? AND expires_at > ?",
		tokenHash, s.now().Unix()).Scan(&uid)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, errNoSession
	}
	if err != nil {
		return nil, err
	}
	return &Session{UserID: uid, tokenHash: tokenHash}, nil
}

// create inserts a new session for userID inside tx and returns its token.
// It also deletes the session being replaced (if any) and expired sessions.
func (s *sessions) create(ctx context.Context, tx *sql.Tx, userID int64, replace []byte) (string, error) {
	now := s.now().Unix()
	if replace != nil {
		if _, err := tx.ExecContext(ctx, "DELETE FROM sessions WHERE token_hash = ?", replace); err != nil {
			return "", err
		}
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM sessions WHERE expires_at <= ?", now); err != nil {
		return "", err
	}
	tok, hash, err := newToken()
	if err != nil {
		return "", err
	}
	_, err = tx.ExecContext(ctx,
		"INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
		hash, userID, now, now+int64(SessionTTL/time.Second))
	if err != nil {
		return "", err
	}
	return tok, nil
}

func (s *sessions) delete(ctx context.Context, tokenHash []byte) error {
	_, err := s.db.Writer.ExecContext(ctx, "DELETE FROM sessions WHERE token_hash = ?", tokenHash)
	return err
}

// require is the session middleware: 401 unauthorized without a live
// session; otherwise the Session is put in the request context and the user
// ID recorded for the access log.
func (s *sessions) require(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "private, no-store")
		hash, ok := tokenFromRequest(r)
		if !ok {
			apperr.Write(w, errNoSession)
			return
		}
		sess, err := s.lookup(r.Context(), hash)
		if err != nil {
			if !errors.Is(err, errNoSession) {
				s.logger.ErrorContext(r.Context(), "session lookup failed", "request_id", reqid.FromContext(r.Context()), "err", err)
			}
			apperr.Write(w, err)
			return
		}
		httpx.SetUserID(r.Context(), sess.UserID)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), sessionKey{}, sess)))
	})
}

// RequireSession returns middleware for routes marked with the `session`
// security scheme: requests without a live session get 401 unauthorized;
// otherwise UserID and SessionKey work on the request. For use by other
// packages' Register functions (ratings, recommend, ...).
func RequireSession(d *app.Deps) func(http.Handler) http.Handler {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	s := &sessions{db: d.DB, now: time.Now, logger: logger}
	return s.require
}
