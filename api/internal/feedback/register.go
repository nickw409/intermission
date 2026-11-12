package feedback

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"net/http"
	"slices"
	"strconv"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/ratelimit"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// MaxNotInterested is how many "not interested" movies a user keeps.
const MaxNotInterested = db.MaxNotInterested

// Actions in the FeedbackRequest schema.
const (
	ActionImpression    = "impression"
	ActionClick         = "click"
	ActionNotInterested = "not_interested"
)

// Public errors. Messages match contracts/fixtures/public/postFeedback.json.
var (
	errBadAction  = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "action must be impression, click or not_interested")
	errBadMovieID = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "movieId must be a positive integer")
	errBadModel   = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "model must be a known model name")
	errNoUser     = apperr.New(http.StatusUnauthorized, apperr.CodeUnauthorized, "not logged in")
)

// request is the FeedbackRequest schema. Pointers tell a missing field apart
// from a zero value.
type request struct {
	MovieID *int64  `json:"movieId"`
	Model   *string `json:"model"`
	Action  *string `json:"action"`
}

// Register adds this package's routes to mux. Called once from main.go.
//
//	POST   /api/feedback                             (session, 60/min per session and per user)
//	GET    /api/feedback/not-interested              (session)
//	DELETE /api/feedback/not-interested/{movieId}    (session, 60/min per user; undo)
//
// The per-user bucket has the same shape as the per-session one, so more
// sessions on one account don't add up to more writes.
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, time.Now)
}

type handler struct {
	db     *db.DB
	movies *moviecache.Cache
	logger *slog.Logger
	now    func() time.Time
}

// NotInterested is the NotInterested schema in contracts/public.yaml.
type NotInterested struct {
	MovieID  int64  `json:"movieId"`
	Title    string `json:"title"`
	Year     *int   `json:"year"`
	HiddenAt int64  `json:"hidden_at"` // Unix seconds
}

// NotInterestedList is the NotInterestedList schema.
type NotInterestedList struct {
	Items []NotInterested `json:"items"`
}

func register(mux *http.ServeMux, d *app.Deps, now func() time.Time) {
	registerWith(mux, d, now, nil)
}

// registerWith is register with a test hook that receives the undo limiter.
func registerWith(mux *http.ServeMux, d *app.Deps, now func() time.Time, observe func(undo *ratelimit.Keyed)) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	h := &handler{db: d.DB, movies: d.Movies, logger: logger, now: now}
	session := auth.RequireSession(d)
	clock := ratelimit.WithClock(now)
	perSession := ratelimit.Middleware(ratelimit.NewKeyed(ratelimit.FeedbackPerSession, clock), auth.SessionKey)
	userBuckets := ratelimit.NewKeyed(ratelimit.FeedbackPerSession, clock)
	perUser := ratelimit.Middleware(userBuckets, userKey)
	undoBuckets := ratelimit.NewKeyed(ratelimit.DeletePerUser, clock)
	perUserUndo := ratelimit.Middleware(undoBuckets, undoKey)
	if observe != nil {
		observe(undoBuckets)
	}
	// A deleted account's buckets go with it (its IDs are never reused).
	d.DB.OnUserDeleted(func(uid int64) {
		userBuckets.Retire(userBucket(uid))
		undoBuckets.Retire(undoBucket(uid))
	})
	mux.Handle("POST /api/feedback", session(perSession(perUser(http.HandlerFunc(h.post)))))
	mux.Handle("GET /api/feedback/not-interested", session(http.HandlerFunc(h.listNotInterested)))
	mux.Handle("DELETE /api/feedback/not-interested/{movieId}", session(perUserUndo(http.HandlerFunc(h.undoNotInterested))))
}

// userKey is the per-user rate-limit key.
func userKey(r *http.Request) (string, bool) {
	uid, ok := auth.UserID(r.Context())
	if !ok {
		return "", false
	}
	return userBucket(uid), true
}

func userBucket(uid int64) string { return "feedback-user:" + strconv.FormatInt(uid, 10) }

// undoKey is the per-user key for the undo limiter, a separate budget from
// POST /api/feedback.
func undoKey(r *http.Request) (string, bool) {
	uid, ok := auth.UserID(r.Context())
	if !ok {
		return "", false
	}
	return undoBucket(uid), true
}

func undoBucket(uid int64) string { return "feedback-undo-user:" + strconv.FormatInt(uid, 10) }

// parse decodes and validates a FeedbackRequest body. Every error is an
// *apperr.Error (400 invalid_json or bad_request). The action is checked
// first, so a bad action is reported even when other fields are also bad.
func parse(body []byte) (movieID int64, model modelclient.ModelName, action string, err error) {
	var req request
	if err := httpx.DecodeJSONBytes(body, &req); err != nil {
		return 0, "", "", err
	}
	if req.Action == nil || !slices.Contains([]string{ActionImpression, ActionClick, ActionNotInterested}, *req.Action) {
		return 0, "", "", errBadAction
	}
	if req.MovieID == nil || *req.MovieID < 1 || *req.MovieID > modelclient.MaxMovieID {
		return 0, "", "", errBadMovieID
	}
	if req.Model == nil || !slices.Contains(modelclient.AllModels, modelclient.ModelName(*req.Model)) {
		return 0, "", "", errBadModel
	}
	return *req.MovieID, modelclient.ModelName(*req.Model), *req.Action, nil
}

// post serves POST /api/feedback: 204 once the signal is recorded.
func (h *handler) post(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	uid, ok := auth.UserID(ctx)
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	body, err := httpx.ReadBody(r)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	movieID, model, action, err := parse(body)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if action == ActionNotInterested {
		if err := h.notInterested(ctx, uid, movieID, model, h.now().Unix()); err != nil {
			h.fail(w, r, "save", err)
			return
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

// notInterested stores one "not interested" row and bumps ratings_version in
// one write transaction (the writer pool has one connection and begins with
// BEGIN IMMEDIATE, so the checks and the insert can't interleave with another
// write). A repeat for the same movie is a no-op; at the cap the oldest row
// makes room.
func (h *handler) notInterested(ctx context.Context, uid, movieID int64, model modelclient.ModelName, now int64) error {
	tx, err := h.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()

	var exists bool
	if err := tx.QueryRowContext(ctx,
		"SELECT EXISTS (SELECT 1 FROM feedback WHERE user_id = ? AND movie_id = ? AND action = 'not_interested')",
		uid, movieID).Scan(&exists); err != nil {
		return err
	}
	if exists {
		return nil
	}
	var n int
	if err := tx.QueryRowContext(ctx,
		"SELECT count(*) FROM feedback WHERE user_id = ? AND action = 'not_interested'", uid).Scan(&n); err != nil {
		return err
	}
	if n >= MaxNotInterested {
		if _, err := tx.ExecContext(ctx,
			`DELETE FROM feedback WHERE rowid IN (
			   SELECT rowid FROM feedback WHERE user_id = ? AND action = 'not_interested'
			   ORDER BY ts ASC, rowid ASC LIMIT ?)`,
			uid, n-MaxNotInterested+1); err != nil {
			return err
		}
	}
	if err := bumpVersion(ctx, tx, uid); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		"INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, ?, 'not_interested', ?)",
		uid, movieID, string(model), now); err != nil {
		return err
	}
	return tx.Commit()
}

// bumpVersion increments users.ratings_version, which keys the
// recommendation cache, so the next list leaves the movie out. No row means
// the account was deleted mid-request.
func bumpVersion(ctx context.Context, tx *sql.Tx, uid int64) error {
	res, err := tx.ExecContext(ctx, "UPDATE users SET ratings_version = ratings_version + 1 WHERE id = ?", uid)
	if err != nil {
		return err
	}
	if n, err := res.RowsAffected(); err != nil {
		return err
	} else if n == 0 {
		return errNoUser
	}
	return nil
}

// listNotInterested serves GET /api/feedback/not-interested: newest first,
// titles from the movie cache only. recs-model is never asked: a movie the
// user hid was on a recommendation list, so it is in the cache; anything else
// (a movie dropped by a new dataset, or an ID a client made up) gets a
// placeholder title. So the list works while recs-model is down, and stored
// IDs can't be used to make this request call recs-model (security review).
func (h *handler) listNotInterested(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	uid, ok := auth.UserID(ctx)
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	rows, err := h.db.Reader.QueryContext(ctx,
		"SELECT movie_id, ts FROM feedback WHERE user_id = ? AND action = 'not_interested' ORDER BY ts DESC, rowid DESC", uid)
	if err != nil {
		h.fail(w, r, "list not interested", err)
		return
	}
	defer rows.Close()
	out := NotInterestedList{Items: []NotInterested{}}
	for rows.Next() {
		var it NotInterested
		if err := rows.Scan(&it.MovieID, &it.HiddenAt); err != nil {
			h.fail(w, r, "scan not interested", err)
			return
		}
		out.Items = append(out.Items, it)
	}
	if err := rows.Err(); err != nil {
		h.fail(w, r, "list not interested", err)
		return
	}
	ids := make([]int64, len(out.Items))
	for i, it := range out.Items {
		ids[i] = it.MovieID
	}
	cached, err := h.movies.Lookup(ctx, ids)
	if err != nil {
		h.fail(w, r, "look up titles", err)
		return
	}
	for i := range out.Items {
		it := &out.Items[i]
		if m, ok := cached[it.MovieID]; ok {
			it.Title, it.Year = m.Title, m.Year
		} else {
			it.Title = "Movie " + strconv.FormatInt(it.MovieID, 10)
		}
	}
	apperr.WriteJSON(w, http.StatusOK, out)
}

// undoNotInterested serves DELETE /api/feedback/not-interested/{movieId}:
// idempotent 204; bumps ratings_version (so the next list can include the
// movie again) only when a row was deleted.
func (h *handler) undoNotInterested(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	uid, ok := auth.UserID(ctx)
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	movieID, err := moviecache.ParseMovieID(r.PathValue("movieId"))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	err = h.undo(ctx, uid, movieID)
	if err != nil {
		h.fail(w, r, "undo not interested", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// undo removes uid's "not interested" row for movieID and bumps
// ratings_version, in one write transaction. Nothing to remove is a no-op,
// checked on the reader first so a repeated undo never takes the single
// writer connection.
func (h *handler) undo(ctx context.Context, uid, movieID int64) error {
	var exists bool
	if err := h.db.Reader.QueryRowContext(ctx,
		"SELECT EXISTS (SELECT 1 FROM feedback WHERE user_id = ? AND movie_id = ? AND action = 'not_interested')",
		uid, movieID).Scan(&exists); err != nil {
		return err
	}
	if !exists {
		return nil
	}
	tx, err := h.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	res, err := tx.ExecContext(ctx,
		"DELETE FROM feedback WHERE user_id = ? AND movie_id = ? AND action = 'not_interested'", uid, movieID)
	if err != nil {
		return err
	}
	if n, err := res.RowsAffected(); err != nil {
		return err
	} else if n == 0 {
		return nil // removed by a concurrent undo since the check
	}
	if err := bumpVersion(ctx, tx, uid); err != nil {
		return err
	}
	return tx.Commit()
}

// fail logs an internal error (never a request body) and writes it: an
// *apperr.Error as is, anything else as 500. A canceled request is not logged.
func (h *handler) fail(w http.ResponseWriter, r *http.Request, what string, err error) {
	var ae *apperr.Error
	if !errors.As(err, &ae) && !errors.Is(err, context.Canceled) {
		h.logger.ErrorContext(r.Context(), "feedback: "+what+" failed", "request_id", reqid.FromContext(r.Context()), "err", err)
	}
	apperr.Write(w, err)
}
