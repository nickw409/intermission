package movies

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"time"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/ratelimit"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Query limits from contracts/public.yaml.
const (
	maxQueryChars   = 100
	maxSearchLimit  = 50
	maxK            = 100
	defaultK        = 20
	maxOnboardingN  = 60
	defaultOnboardN = 30
)

// Public errors. Messages match contracts/fixtures/public.
var (
	errBadQuery      = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "q must be 1-100 characters")
	errBadLimit      = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "limit must be between 1 and 50")
	errBadK          = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "k must be between 1 and 100")
	errBadN          = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "n must be between 1 and 60")
	errSimilarModel  = apperr.New(http.StatusBadRequest, apperr.CodeUnknownModel, "similar supports knn and content")
	errModelNotReady = apperr.New(http.StatusBadRequest, apperr.CodeUnknownModel, "that model is not loaded")
	errNoUser        = apperr.New(http.StatusUnauthorized, apperr.CodeUnauthorized, "not logged in")
)

// Response shapes (contracts/public.yaml).

// MovieSummary is the MovieSummary schema.
type MovieSummary struct {
	MovieID int64    `json:"movieId"`
	Title   string   `json:"title"`
	Year    *int     `json:"year"`
	Genres  []string `json:"genres"`
}

// MovieList is the MovieList schema.
type MovieList struct {
	Items []MovieSummary `json:"items"`
}

// Movie is the Movie schema.
type Movie struct {
	MovieID     int64    `json:"movieId"`
	Title       string   `json:"title"`
	Year        *int     `json:"year"`
	Genres      []string `json:"genres"`
	RatingCount int      `json:"rating_count"`
	MeanRating  *float64 `json:"mean_rating"`
	TMDBID      *int64   `json:"tmdbId"`
	UserRating  *float64 `json:"user_rating"`
}

// SimilarItem is one entry of SimilarResponse.items.
type SimilarItem struct {
	MovieID    int64   `json:"movieId"`
	Title      string  `json:"title"`
	Year       *int    `json:"year"`
	Similarity float64 `json:"similarity"`
}

// SimilarResponse is the SimilarResponse schema.
type SimilarResponse struct {
	Model        modelclient.ModelName `json:"model"`
	ModelVersion string                `json:"model_version"`
	MovieID      int64                 `json:"movieId"`
	Items        []SimilarItem         `json:"items"`
}

// Register adds this package's routes to mux. Called once from main.go.
//
//	GET /api/movies/search?q=&limit=
//	GET /api/movies/{movieId}              (session optional)
//	GET /api/movies/{movieId}/similar?model=&k=
//	GET /api/onboarding?n=                 (session)
//	GET /api/models
//
// The four anonymous catalog routes share one global token bucket,
// CatalogGlobal; onboarding needs a session and is not charged to it.
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, CatalogGlobal)
}

// CatalogGlobal is the shared budget of the anonymous catalog routes
// (search, movie, similar, models): 20 requests per second sustained, bursts
// of 100. Each request costs at most two recs-model calls (search or similar,
// then a batch lookup for uncached results) and at most one short cache
// write, so an anonymous flood can't saturate recs-model or the single
// SQLite writer. A typing user's search box stays far below it (the web UI
// debounces keystrokes). Over budget: 429 rate_limited with Retry-After.
var CatalogGlobal = ratelimit.Config{Every: 50 * time.Millisecond, Burst: 100}

func register(mux *http.ServeMux, d *app.Deps, budget ratelimit.Config) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	h := &handler{db: d.DB, model: d.Model, movies: d.Movies, logger: logger}
	limit := globalLimit(ratelimit.NewGlobal(budget))
	mux.Handle("GET /api/movies/search", limit(http.HandlerFunc(h.search)))
	mux.Handle("GET /api/movies/{movieId}", limit(optionalSession(d)(http.HandlerFunc(h.movie))))
	mux.Handle("GET /api/movies/{movieId}/similar", limit(http.HandlerFunc(h.similar)))
	mux.Handle("GET /api/onboarding", auth.RequireSession(d)(http.HandlerFunc(h.onboarding)))
	mux.Handle("GET /api/models", limit(http.HandlerFunc(h.models)))
}

// globalLimit charges every request to g before anything else runs.
func globalLimit(g *ratelimit.Global) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if ok, retry := g.Allow(); !ok {
				ratelimit.WriteLimited(w, retry)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

type handler struct {
	db     *db.DB
	model  modelclient.Client
	movies *moviecache.Cache
	logger *slog.Logger
}

// search serves GET /api/movies/search. The query goes to recs-model as an
// opaque string (it is never part of any SQL statement); the IDs that come
// back are described from the movie cache.
func (h *handler) search(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	text := q.Get("q")
	if !utf8.ValidString(text) {
		apperr.Write(w, errBadQuery)
		return
	}
	if n := utf8.RuneCountInString(text); n < 1 || n > maxQueryChars {
		apperr.Write(w, errBadQuery)
		return
	}
	limit, ok := intParam(q, "limit", 0, 1, maxSearchLimit) // 0: recs-model's default (20)
	if !ok {
		apperr.Write(w, errBadLimit)
		return
	}
	ids, _, err := h.model.Search(r.Context(), text, limit)
	if err != nil {
		h.modelError(w, r, "search", err)
		return
	}
	items, err := h.summaries(r.Context(), ids.MovieIDs, nil)
	if err != nil {
		h.modelError(w, r, "search: describe results", err)
		return
	}
	apperr.WriteJSON(w, http.StatusOK, MovieList{Items: items})
}

// movie serves GET /api/movies/{movieId}. The row is refreshed from
// recs-model (current counts) and falls back to the cache when it is down.
// With a session, user_rating is filled in and the response is marked
// private.
func (h *handler) movie(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	id, err := moviecache.ParseMovieID(r.PathValue("movieId"))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	uid, hasSession := auth.UserID(ctx)
	w.Header().Set("Vary", "Cookie")
	if hasSession {
		w.Header().Set("Cache-Control", "private, no-store")
	}
	m, found, err := h.movies.Refresh(ctx, id)
	if err != nil {
		h.modelError(w, r, "movie", err)
		return
	}
	if !found {
		apperr.Write(w, moviecache.ErrMovieNotFound)
		return
	}
	out := Movie{
		MovieID: m.MovieID, Title: m.Title, Year: m.Year, Genres: nonNil(m.Genres),
		RatingCount: m.RatingCount, MeanRating: m.MeanRating, TMDBID: m.TMDBID,
	}
	if hasSession {
		var rating float64
		err := h.db.Reader.QueryRowContext(ctx,
			"SELECT rating FROM ratings WHERE user_id = ? AND movie_id = ?", uid, id).Scan(&rating)
		switch {
		case err == nil:
			out.UserRating = &rating
		case !errors.Is(err, sql.ErrNoRows):
			h.internal(w, r, "movie: read user rating", err)
			return
		}
	}
	apperr.WriteJSON(w, http.StatusOK, out)
}

// similar serves GET /api/movies/{movieId}/similar.
func (h *handler) similar(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	id, err := moviecache.ParseMovieID(r.PathValue("movieId"))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	q := r.URL.Query()
	model := modelclient.ModelKNN
	if q.Has("model") {
		switch m := modelclient.ModelName(q.Get("model")); m {
		case modelclient.ModelKNN, modelclient.ModelContent:
			model = m
		default:
			apperr.Write(w, errSimilarModel)
			return
		}
	}
	k, ok := intParam(q, "k", defaultK, 1, maxK)
	if !ok {
		apperr.Write(w, errBadK)
		return
	}
	resp, meta, err := h.model.Similar(ctx, id, model, k)
	if err != nil {
		h.modelError(w, r, "similar", err)
		return
	}
	// The movie itself is looked up with its neighbours so the movie page
	// that follows is already cached.
	ids := make([]int64, 0, len(resp.Items)+1)
	ids = append(ids, id)
	for _, it := range resp.Items {
		ids = append(ids, it.MovieID)
	}
	res, err := h.movies.Get(ctx, ids)
	if describeFailed(res, err) {
		h.modelError(w, r, "similar: describe results", err)
		return
	}
	out := SimilarResponse{Model: model, ModelVersion: meta.ModelVersion, MovieID: id, Items: []SimilarItem{}}
	for _, it := range resp.Items {
		if m, ok := res.Movies[it.MovieID]; ok {
			out.Items = append(out.Items, SimilarItem{MovieID: it.MovieID, Title: m.Title, Year: m.Year, Similarity: it.Similarity})
		}
	}
	apperr.WriteJSON(w, http.StatusOK, out)
}

// onboarding serves GET /api/onboarding: popular, genre-diverse movies the
// user hasn't rated (rated movies are sent as exclude and filtered again).
func (h *handler) onboarding(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	uid, ok := auth.UserID(ctx)
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	n, ok := intParam(r.URL.Query(), "n", defaultOnboardN, 1, maxOnboardingN)
	if !ok {
		apperr.Write(w, errBadN)
		return
	}
	rated, err := h.ratedMovies(ctx, uid)
	if err != nil {
		h.internal(w, r, "onboarding: read ratings", err)
		return
	}
	resp, _, err := h.model.Onboarding(ctx, modelclient.OnboardingRequest{N: n, Exclude: rated})
	if err != nil {
		h.modelError(w, r, "onboarding", err)
		return
	}
	skip := make(map[int64]bool, len(rated))
	for _, id := range rated {
		skip[id] = true
	}
	items, err := h.summaries(ctx, resp.MovieIDs, skip)
	if err != nil {
		h.modelError(w, r, "onboarding: describe results", err)
		return
	}
	apperr.WriteJSON(w, http.StatusOK, MovieList{Items: items})
}

// ratedMovies returns the IDs the user has rated, newest first (at most
// 5,000, within recs-model's exclude limit of 10,000).
func (h *handler) ratedMovies(ctx context.Context, uid int64) ([]int64, error) {
	rows, err := h.db.Reader.QueryContext(ctx,
		"SELECT movie_id FROM ratings WHERE user_id = ? ORDER BY updated_at DESC, movie_id ASC", uid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

// models serves GET /api/models.
func (h *handler) models(w http.ResponseWriter, r *http.Request) {
	resp, _, err := h.model.Models(r.Context())
	if err != nil {
		h.modelError(w, r, "models", err)
		return
	}
	if resp.Models == nil {
		resp.Models = []modelclient.ModelName{}
	}
	apperr.WriteJSON(w, http.StatusOK, resp)
}

// summaries describes ids from the movie cache, in order, leaving out IDs in
// skip and IDs recs-model doesn't know. It fails only if some ID could not
// be described because recs-model could not be asked.
func (h *handler) summaries(ctx context.Context, ids []int64, skip map[int64]bool) ([]MovieSummary, error) {
	res, err := h.movies.Get(ctx, ids)
	if describeFailed(res, err) {
		return nil, err
	}
	out := make([]MovieSummary, 0, len(ids))
	seen := make(map[int64]bool, len(ids))
	for _, id := range ids {
		m, ok := res.Movies[id]
		if !ok || skip[id] || seen[id] {
			continue
		}
		seen[id] = true
		out = append(out, MovieSummary{MovieID: id, Title: m.Title, Year: m.Year, Genres: nonNil(m.Genres)})
	}
	return out, nil
}

// describeFailed reports whether a cache lookup failed in a way the caller
// must report: some ID could not be checked, or the database read failed
// (nothing came back at all).
func describeFailed(res moviecache.Result, err error) bool {
	return err != nil && (len(res.Unchecked) > 0 || len(res.Movies) == 0)
}

// modelError maps a recs-model (or cache) error to the public error.
func (h *handler) modelError(w http.ResponseWriter, r *http.Request, what string, err error) {
	var se *modelclient.StatusError
	switch {
	case errors.Is(err, modelclient.ErrUnavailable):
		apperr.Write(w, moviecache.ErrModelUnavailable)
	case errors.As(err, &se) && se.Status == http.StatusNotFound:
		apperr.Write(w, moviecache.ErrMovieNotFound)
	case errors.As(err, &se) && se.Code == modelclient.ErrorCodeUnknownModel:
		apperr.Write(w, errModelNotReady)
	default:
		h.internal(w, r, what, err)
	}
}

// internal logs err (never request bodies or tokens) and writes 500.
func (h *handler) internal(w http.ResponseWriter, r *http.Request, what string, err error) {
	h.logger.ErrorContext(r.Context(), "movies: "+what+" failed", "request_id", reqid.FromContext(r.Context()), "err", err)
	apperr.WriteError(w, http.StatusInternalServerError, apperr.CodeInternal, "internal error")
}

// intParam parses an optional integer query parameter: ASCII digits only,
// within [lo, hi]. A missing parameter yields def.
func intParam(q map[string][]string, name string, def, lo, hi int) (int, bool) {
	vals, present := q[name]
	if !present {
		return def, true
	}
	if len(vals) != 1 || vals[0] == "" || len(vals[0]) > 4 {
		return 0, false
	}
	for i := 0; i < len(vals[0]); i++ {
		if vals[0][i] < '0' || vals[0][i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.Atoi(vals[0])
	if err != nil || n < lo || n > hi {
		return 0, false
	}
	return n, true
}

func nonNil(s []string) []string {
	if s == nil {
		return []string{}
	}
	return s
}

// optionalSession attaches the caller's session to the request context when
// the request carries a valid session cookie, and otherwise lets the request
// through anonymously (a dead or malformed cookie is simply ignored). It
// reuses auth.RequireSession for the actual validation, so token parsing and
// lookup stay in one place: the request is first run through RequireSession
// with a throwaway response writer; if RequireSession calls the inner
// handler, the real handler runs with the session context and the real
// writer, otherwise it runs anonymously.
func optionalSession(d *app.Deps) func(http.Handler) http.Handler {
	require := auth.RequireSession(d)
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if _, err := r.Cookie(auth.CookieName); err != nil {
				next.ServeHTTP(w, r)
				return
			}
			served := false
			probe := require(http.HandlerFunc(func(_ http.ResponseWriter, withSession *http.Request) {
				served = true
				next.ServeHTTP(w, withSession)
			}))
			probe.ServeHTTP(&discardWriter{header: http.Header{}}, r)
			if !served {
				next.ServeHTTP(w, r)
			}
		})
	}
}

// discardWriter swallows RequireSession's 401 response.
type discardWriter struct{ header http.Header }

func (d *discardWriter) Header() http.Header         { return d.header }
func (d *discardWriter) Write(b []byte) (int, error) { return len(b), nil }
func (d *discardWriter) WriteHeader(int)             {}
