package auth

import (
	"context"
	"database/sql"
	"errors"
	"net/http"
	"strings"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/ratelimit"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Me is the account summary returned by signup, login and GET /api/me.
type Me struct {
	Username    string `json:"username"`
	RatingCount int64  `json:"rating_count"`
	CreatedAt   int64  `json:"created_at"` // Unix seconds
}

// Field rules from contracts/public.yaml (SignupRequest, LoginRequest).
const (
	minUsername      = 3
	maxUsername      = 32
	minPassword      = 10
	maxPassword      = 128
	maxInviteCode    = 64
	maxLoginUsername = 64
)

// Public errors. Messages match contracts/fixtures/public.
var (
	errBadUsername   = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "username must be 3-32 characters of A-Z a-z 0-9 _ . -")
	errBadPassword   = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "password must be 10-128 characters")
	errBadInviteLen  = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "invite_code must be at most 64 characters")
	errLoginFields   = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "username (at most 64 characters) and password (at most 128 characters) are required")
	errInvalidInvite = apperr.New(http.StatusBadRequest, apperr.CodeInvalidInvite, "invite code is invalid")
	errUsernameTaken = apperr.New(http.StatusConflict, apperr.CodeUsernameTaken, "username is taken")
	errInvalidCreds  = apperr.New(http.StatusUnauthorized, apperr.CodeInvalidCredentials, "invalid username or password")
)

// ---------------------------------------------------------------- decoding

type signupRequest struct {
	Username   *string `json:"username"`
	Password   *string `json:"password"`
	InviteCode *string `json:"invite_code"`
}

type signupInput struct {
	username, password, inviteCode string
}

// parseSignup decodes and validates a signup body (check-order step 1).
func parseSignup(data []byte) (signupInput, error) {
	var req signupRequest
	if err := httpx.DecodeJSONBytes(data, &req); err != nil {
		return signupInput{}, err
	}
	if req.Username == nil || !validUsername(*req.Username) {
		return signupInput{}, errBadUsername
	}
	if req.Password == nil {
		return signupInput{}, errBadPassword
	}
	if n := utf8.RuneCountInString(*req.Password); n < minPassword || n > maxPassword {
		return signupInput{}, errBadPassword
	}
	in := signupInput{username: *req.Username, password: *req.Password}
	if req.InviteCode != nil {
		if utf8.RuneCountInString(*req.InviteCode) > maxInviteCode {
			return signupInput{}, errBadInviteLen
		}
		in.inviteCode = *req.InviteCode
	}
	return in, nil
}

// validUsername: 3-32 characters of [A-Za-z0-9_.-].
func validUsername(u string) bool {
	if len(u) < minUsername || len(u) > maxUsername {
		return false
	}
	for i := 0; i < len(u); i++ {
		c := u[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '_', c == '.', c == '-':
		default:
			return false
		}
	}
	return true
}

type loginRequest struct {
	Username *string `json:"username"`
	Password *string `json:"password"`
}

type loginInput struct {
	username, password string
}

// parseLogin decodes and validates a login body. Only presence and maximum
// lengths are checked: the account rules are not revealed at login.
func parseLogin(data []byte) (loginInput, error) {
	var req loginRequest
	if err := httpx.DecodeJSONBytes(data, &req); err != nil {
		return loginInput{}, err
	}
	if req.Username == nil || req.Password == nil ||
		utf8.RuneCountInString(*req.Username) > maxLoginUsername ||
		utf8.RuneCountInString(*req.Password) > maxPassword {
		return loginInput{}, errLoginFields
	}
	return loginInput{username: *req.Username, password: *req.Password}, nil
}

// rateKey is the per-account limiter key: the submitted username, case-folded
// the way the users table compares names (COLLATE NOCASE folds ASCII only).
func rateKey(username string) string {
	return strings.ToLower(username)
}

// ---------------------------------------------------------------- handlers

func (s *service) fail(w http.ResponseWriter, r *http.Request, err error) {
	var pub *apperr.Error
	if !errors.As(err, &pub) {
		s.logger.ErrorContext(r.Context(), "auth: internal error",
			"request_id", reqid.FromContext(r.Context()), "route", r.Pattern, "err", err)
	}
	apperr.Write(w, err)
}

func noStore(w http.ResponseWriter) { w.Header().Set("Cache-Control", "no-store") }

// signup follows the check order frozen in contracts/public.yaml:
//  1. body shape and field rules → 400 bad_request / invalid_json;
//  2. invite mode: invite checked without consuming it → 400 invalid_invite,
//     before any username lookup;
//  3. username → 409 username_taken (invite not consumed);
//  4. one transaction: consume the invite (re-checked atomically), create the
//     user, create the session.
//
// The global cap (429) is charged once the body is well formed, so junk
// requests can't spend the budget of real signups.
func (s *service) signup(w http.ResponseWriter, r *http.Request) {
	noStore(w)
	data, err := httpx.ReadBody(r)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	in, err := parseSignup(data)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if ok, retry := s.signupGlobal.Allow(); !ok {
		ratelimit.WriteLimited(w, retry)
		return
	}
	ctx := r.Context()
	inviteMode := s.mode != app.SignupOpen
	if inviteMode {
		if err := s.invites.Check(ctx, in.inviteCode); err != nil {
			s.fail(w, r, inviteErr(err))
			return
		}
	}
	taken, err := s.usernameTaken(ctx, s.db.Reader, in.username)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if taken {
		s.fail(w, r, errUsernameTaken)
		return
	}

	pwHash, err := s.hasher.Hash(ctx, in.password)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	replace, _ := tokenFromRequest(r)
	now := s.now().Unix()

	tx, err := s.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	defer func() { _ = tx.Rollback() }()
	// Re-check both inside the (serialized) write transaction: another signup
	// may have used the last invite slot or the name since the checks above.
	if inviteMode {
		if err := s.invites.Redeem(ctx, tx, in.inviteCode); err != nil {
			s.fail(w, r, inviteErr(err))
			return
		}
	}
	if taken, err := s.usernameTaken(ctx, tx, in.username); err != nil || taken {
		if err == nil {
			err = errUsernameTaken
		}
		s.fail(w, r, err)
		return
	}
	res, err := tx.ExecContext(ctx,
		"INSERT INTO users (username, pw_hash, created_at) VALUES (?, ?, ?)", in.username, pwHash, now)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	uid, err := res.LastInsertId()
	if err != nil {
		s.fail(w, r, err)
		return
	}
	token, err := s.sessions.create(ctx, tx, uid, replace)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if err := tx.Commit(); err != nil {
		s.fail(w, r, err)
		return
	}
	httpx.SetUserID(ctx, uid)
	http.SetCookie(w, sessionCookie(token))
	apperr.WriteJSON(w, http.StatusCreated, Me{Username: in.username, RatingCount: 0, CreatedAt: now})
}

func inviteErr(err error) error {
	if errors.Is(err, invites.ErrInvalid) {
		return errInvalidInvite
	}
	return err
}

type queryer interface {
	QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row
}

func (s *service) usernameTaken(ctx context.Context, q queryer, username string) (bool, error) {
	var one int
	err := q.QueryRowContext(ctx, "SELECT 1 FROM users WHERE username = ?", username).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	return err == nil, err
}

// login: body → global cap → per-account cap (keyed on the case-folded
// submitted name, existing or not) → exactly one Argon2id verification
// (against a dummy hash for unknown users) → new session, replacing the one
// presented, if any.
func (s *service) login(w http.ResponseWriter, r *http.Request) {
	noStore(w)
	data, err := httpx.ReadBody(r)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	in, err := parseLogin(data)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if ok, retry := s.loginGlobal.Allow(); !ok {
		ratelimit.WriteLimited(w, retry)
		return
	}
	if ok, retry := s.loginPerAccount.Allow(rateKey(in.username)); !ok {
		ratelimit.WriteLimited(w, retry)
		return
	}
	ctx := r.Context()

	var uid int64
	var stored string
	err = s.db.Reader.QueryRowContext(ctx,
		"SELECT id, pw_hash FROM users WHERE username = ?", in.username).Scan(&uid, &stored)
	known := err == nil
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		s.fail(w, r, err)
		return
	}
	if !known {
		stored = s.dummyHash
	}
	match, err := s.hasher.Verify(ctx, in.password, stored)
	if errors.Is(err, errBadHash) {
		// A corrupt row: log it (never the hash or password) and answer like
		// any failed login, after the same single hash of work.
		s.logger.ErrorContext(ctx, "auth: stored password hash is malformed",
			"request_id", reqid.FromContext(ctx), "user_id", uid)
		_, _ = s.hasher.Verify(ctx, in.password, s.dummyHash)
		match, err = false, nil
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if !known || !match {
		s.fail(w, r, errInvalidCreds)
		return
	}

	replace, _ := tokenFromRequest(r)
	tx, err := s.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	defer func() { _ = tx.Rollback() }()
	token, err := s.sessions.create(ctx, tx, uid, replace)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if err := tx.Commit(); err != nil {
		s.fail(w, r, err)
		return
	}
	httpx.SetUserID(ctx, uid)
	me, err := s.me(ctx, uid)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	http.SetCookie(w, sessionCookie(token))
	apperr.WriteJSON(w, http.StatusOK, me)
}

func (s *service) logout(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(*Session)
	if err := s.sessions.delete(r.Context(), sess.tokenHash); err != nil {
		s.fail(w, r, err)
		return
	}
	http.SetCookie(w, clearedCookie())
	w.WriteHeader(http.StatusNoContent)
}

func (s *service) getMe(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(*Session)
	me, err := s.me(r.Context(), sess.UserID)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	apperr.WriteJSON(w, http.StatusOK, me)
}

// deleteMe removes the user; ON DELETE CASCADE removes their sessions,
// ratings and feedback in the same statement.
func (s *service) deleteMe(w http.ResponseWriter, r *http.Request) {
	sess := r.Context().Value(sessionKey{}).(*Session)
	if _, err := s.db.Writer.ExecContext(r.Context(), "DELETE FROM users WHERE id = ?", sess.UserID); err != nil {
		s.fail(w, r, err)
		return
	}
	http.SetCookie(w, clearedCookie())
	w.WriteHeader(http.StatusNoContent)
}

func (s *service) me(ctx context.Context, uid int64) (Me, error) {
	var m Me
	err := s.db.Reader.QueryRowContext(ctx,
		"SELECT username, created_at, (SELECT count(*) FROM ratings WHERE user_id = users.id) FROM users WHERE id = ?",
		uid).Scan(&m.Username, &m.CreatedAt, &m.RatingCount)
	if errors.Is(err, sql.ErrNoRows) {
		return Me{}, errNoSession // deleted between the session check and now
	}
	return m, err
}
