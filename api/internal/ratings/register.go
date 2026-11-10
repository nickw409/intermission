package ratings

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"net/http"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/httpx"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// MaxRatingsPerUser is the per-user cap (spec: "Input limits").
const MaxRatingsPerUser = 5000

// Public errors. Messages match contracts/fixtures/public.
var (
	errBadRating      = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "rating must be 0.5-5.0 in 0.5 steps")
	errTooManyRatings = apperr.New(http.StatusConflict, apperr.CodeTooManyRatings, "at most 5000 ratings per user")
	errNoUser         = apperr.New(http.StatusUnauthorized, apperr.CodeUnauthorized, "not logged in")
)

// Rating is the Rating schema in contracts/public.yaml.
type Rating struct {
	MovieID   int64   `json:"movieId"`
	Title     string  `json:"title"`
	Year      *int    `json:"year"`
	Rating    float64 `json:"rating"`
	UpdatedAt int64   `json:"updated_at"` // Unix seconds
}

// RatingList is the RatingList schema.
type RatingList struct {
	Items []Rating `json:"items"`
}

// putRequest is the PutRatingRequest schema. A pointer tells a missing or
// null rating apart from 0.
type putRequest struct {
	Rating *float64 `json:"rating"`
}

// Register adds this package's routes to mux. Called once from main.go.
//
//	GET    /api/ratings            (session)
//	PUT    /api/ratings/{movieId}  (session)
//	DELETE /api/ratings/{movieId}  (session)
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, time.Now)
}

type handler struct {
	db     *db.DB
	movies *moviecache.Cache
	logger *slog.Logger
	now    func() time.Time
}

func register(mux *http.ServeMux, d *app.Deps, now func() time.Time) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	h := &handler{db: d.DB, movies: d.Movies, logger: logger, now: now}
	session := auth.RequireSession(d)
	mux.Handle("GET /api/ratings", session(http.HandlerFunc(h.list)))
	mux.Handle("PUT /api/ratings/{movieId}", session(http.HandlerFunc(h.put)))
	mux.Handle("DELETE /api/ratings/{movieId}", session(http.HandlerFunc(h.delete)))
}

// parsePut decodes and validates a PutRatingRequest body. Every error is an
// *apperr.Error (400 invalid_json or bad_request).
func parsePut(body []byte) (float64, error) {
	var req putRequest
	if err := httpx.DecodeJSONBytes(body, &req); err != nil {
		return 0, err
	}
	if req.Rating == nil || !ValidRating(*req.Rating) {
		return 0, errBadRating
	}
	return *req.Rating, nil
}

// ValidRating reports whether r is 0.5..5.0 in exact 0.5 steps.
func ValidRating(r float64) bool {
	if math.IsNaN(r) || r < 0.5 || r > 5 {
		return false
	}
	return r*2 == math.Trunc(r*2)
}

// list serves GET /api/ratings: newest first, titles from the movie cache.
// recs-model is asked only for movies missing from the cache; if it is down
// those get a placeholder title, so the list always works.
func (h *handler) list(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	uid, ok := auth.UserID(ctx)
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	rows, err := h.db.Reader.QueryContext(ctx,
		"SELECT movie_id, rating, updated_at FROM ratings WHERE user_id = ? ORDER BY updated_at DESC, movie_id ASC", uid)
	if err != nil {
		h.fail(w, r, "list ratings", err)
		return
	}
	defer rows.Close()
	out := RatingList{Items: []Rating{}}
	for rows.Next() {
		var it Rating
		if err := rows.Scan(&it.MovieID, &it.Rating, &it.UpdatedAt); err != nil {
			h.fail(w, r, "scan rating", err)
			return
		}
		out.Items = append(out.Items, it)
	}
	if err := rows.Err(); err != nil {
		h.fail(w, r, "list ratings", err)
		return
	}

	ids := make([]int64, len(out.Items))
	for i, it := range out.Items {
		ids[i] = it.MovieID
	}
	res, err := h.movies.Get(ctx, ids)
	if err != nil {
		h.logger.WarnContext(ctx, "ratings list: some titles unavailable", "request_id", reqid.FromContext(ctx),
			"missing", len(res.Unchecked), "err", err)
	}
	for i := range out.Items {
		it := &out.Items[i]
		if m, ok := res.Movies[it.MovieID]; ok {
			it.Title, it.Year = m.Title, m.Year
		} else {
			it.Title = fmt.Sprintf("Movie %d", it.MovieID)
		}
	}
	apperr.WriteJSON(w, http.StatusOK, out)
}

// put serves PUT /api/ratings/{movieId}. Order of checks: movie ID, body,
// rating value, movie existence (cache, then recs-model), then in one write
// transaction: the 5,000 cap, the upsert and the ratings_version bump.
func (h *handler) put(w http.ResponseWriter, r *http.Request) {
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
	body, err := httpx.ReadBody(r)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	rating, err := parsePut(body)
	if err != nil {
		apperr.Write(w, err)
		return
	}

	movie, err := h.movie(ctx, movieID)
	if err != nil {
		var ae *apperr.Error
		if !errors.As(err, &ae) {
			h.fail(w, r, "check movie", err)
			return
		}
		apperr.Write(w, err)
		return
	}

	now := h.now().Unix()
	if err := h.write(ctx, uid, movieID, rating, now); err != nil {
		var ae *apperr.Error
		if errors.As(err, &ae) {
			apperr.Write(w, err)
			return
		}
		h.fail(w, r, "save rating", err)
		return
	}
	apperr.WriteJSON(w, http.StatusOK, Rating{MovieID: movieID, Title: movie.Title, Year: movie.Year, Rating: rating, UpdatedAt: now})
}

// movie returns the cached or freshly fetched metadata for id, or
// ErrMovieNotFound / ErrModelUnavailable (an *apperr.Error), or another
// error for a failure that is ours.
func (h *handler) movie(ctx context.Context, id int64) (modelclient.MovieMeta, error) {
	res, err := h.movies.Get(ctx, []int64{id})
	if m, ok := res.Movies[id]; ok {
		return m, nil
	}
	if len(res.Unknown) > 0 {
		return modelclient.MovieMeta{}, moviecache.ErrMovieNotFound
	}
	if errors.Is(err, modelclient.ErrUnavailable) {
		return modelclient.MovieMeta{}, moviecache.ErrModelUnavailable
	}
	if err == nil {
		err = errors.New("movie neither found nor reported missing")
	}
	return modelclient.MovieMeta{}, err
}

// write upserts one rating and bumps ratings_version in one transaction.
// The writer pool has one connection and starts transactions with BEGIN
// IMMEDIATE, so the cap check and the insert can't interleave with another
// write.
func (h *handler) write(ctx context.Context, uid, movieID int64, rating float64, now int64) error {
	tx, err := h.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()

	var exists int
	if err := tx.QueryRowContext(ctx,
		"SELECT count(*) FROM ratings WHERE user_id = ? AND movie_id = ?", uid, movieID).Scan(&exists); err != nil {
		return err
	}
	if exists == 0 {
		var n int
		if err := tx.QueryRowContext(ctx, "SELECT count(*) FROM ratings WHERE user_id = ?", uid).Scan(&n); err != nil {
			return err
		}
		if n >= MaxRatingsPerUser {
			return errTooManyRatings
		}
	}
	if err := bump(ctx, tx, uid); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)
		 ON CONFLICT (user_id, movie_id) DO UPDATE SET rating = excluded.rating, updated_at = excluded.updated_at`,
		uid, movieID, rating, now); err != nil {
		return err
	}
	return tx.Commit()
}

// delete serves DELETE /api/ratings/{movieId}: idempotent 204; bumps
// ratings_version only when a row was deleted.
func (h *handler) delete(w http.ResponseWriter, r *http.Request) {
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
	err = func() error {
		tx, err := h.db.Writer.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer func() { _ = tx.Rollback() }()
		res, err := tx.ExecContext(ctx, "DELETE FROM ratings WHERE user_id = ? AND movie_id = ?", uid, movieID)
		if err != nil {
			return err
		}
		n, err := res.RowsAffected()
		if err != nil {
			return err
		}
		if n == 0 {
			return nil
		}
		if err := bump(ctx, tx, uid); err != nil {
			return err
		}
		return tx.Commit()
	}()
	if err != nil {
		var ae *apperr.Error
		if errors.As(err, &ae) {
			apperr.Write(w, err)
			return
		}
		h.fail(w, r, "delete rating", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// bump is the ratings_version step of every write. It is a variable only so
// tests can make it fail and check that the rating write rolls back with it.
var bump = bumpVersion

// bumpVersion increments users.ratings_version (it keys the recommendation
// cache). No row means the account was deleted mid-request.
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

// fail logs an internal error (never the request body) and writes 500.
func (h *handler) fail(w http.ResponseWriter, r *http.Request, what string, err error) {
	h.logger.ErrorContext(r.Context(), "ratings: "+what+" failed", "request_id", reqid.FromContext(r.Context()), "err", err)
	apperr.Write(w, err)
}
