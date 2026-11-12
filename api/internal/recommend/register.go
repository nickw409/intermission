package recommend

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/ratelimit"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Limits and defaults (contracts/public.yaml, spec "HTTP API").
const (
	// ColdStartBelow: users with fewer ratings get popularity results.
	ColdStartBelow = 5
	defaultK       = 20
	maxK           = modelclient.MaxK
	maxCompare     = modelclient.MaxCompare
	maxModelsParam = 64  // characters, compare's models parameter
	maxGenresParam = 200 // characters, the genres parameter
	maxGenres      = modelclient.MaxGenres
	maxYearParam   = 4 // characters, year_from and year_to (whole years 1800-2100)
	maxBecause     = 3 // RecItem.because maxItems
	// Recommendation cache bounds: entries, approximate bytes, and entries
	// per user.
	DefaultCacheEntries = 16384
	DefaultCacheBytes   = 64 << 20
	DefaultCachePerUser = 32
	// maxEchoedName caps a model name repeated back in an error message.
	maxEchoedName = 32
	// modelBudget caps the time one request spends waiting on recs-model
	// across all its calls (version, models, recommend, batch), so a slow
	// model still yields 503 within a second.
	modelBudget = 900 * time.Millisecond
)

// Public errors. Messages match contracts/fixtures/public.
var (
	errBadK      = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "k must be between 1 and 100")
	errBadModels = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "models must list 1-5 distinct model names")
	errBadGenres = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "genres must list up to 19 distinct genre names")
	errBadYears  = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "year_from and year_to must be whole years from 1800 to 2100, with year_from no later than year_to")
	errNoUser    = apperr.New(http.StatusUnauthorized, apperr.CodeUnauthorized, "not logged in")
)

func errUnknownModel(name string) error {
	if len(name) > maxEchoedName {
		name = name[:maxEchoedName] + "..."
	}
	return apperr.New(http.StatusBadRequest, apperr.CodeUnknownModel, fmt.Sprintf("unknown model %q", name))
}

var errModelNotLoaded = apperr.New(http.StatusBadRequest, apperr.CodeUnknownModel, "that model is not loaded")

// Response shapes (contracts/public.yaml).

// Because is the Because schema.
type Because struct {
	MovieID int64   `json:"movieId"`
	Title   string  `json:"title"`
	Weight  float64 `json:"weight"`
}

// RecItem is the RecItem schema.
type RecItem struct {
	MovieID int64     `json:"movieId"`
	Title   string    `json:"title"`
	Year    *int      `json:"year"`
	Score   float64   `json:"score"`
	Because []Because `json:"because"`
}

// RecommendResponse is the RecommendResponse schema.
type RecommendResponse struct {
	Model        modelclient.ModelName `json:"model"`
	ModelVersion string                `json:"model_version"`
	Items        []RecItem             `json:"items"`
	ColdStart    bool                  `json:"cold_start"`
}

// CompareResponse is the CompareResponse schema.
type CompareResponse struct {
	ModelVersion string   `json:"model_version"`
	ColdStart    bool     `json:"cold_start"`
	Results      []result `json:"results"`
}

// Register adds this package's routes to mux. Called once from main.go.
//
//	GET /api/recommend?model=&k=&genres=&year_from=&year_to=                 (session, 30/min per session and per user)
//	GET /api/recommend/compare?models=a,b,c&k=&genres=&year_from=&year_to=   (session, same buckets)
//
// The per-user bucket has the same shape as the per-session one, so logging
// in again for a fresh session doesn't reset the budget.
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, options{})
}

// options are the tunable parts; zero values mean the defaults above.
type options struct {
	cacheEntries int
	cacheBytes   int64
	cachePerUser int
	// observe, if set, receives the cache and the per-user buckets (tests).
	observe func(*lru, *ratelimit.Keyed)
}

func register(mux *http.ServeMux, d *app.Deps, o options) {
	if o.cacheEntries <= 0 {
		o.cacheEntries = DefaultCacheEntries
	}
	if o.cacheBytes <= 0 {
		o.cacheBytes = DefaultCacheBytes
	}
	if o.cachePerUser <= 0 {
		o.cachePerUser = DefaultCachePerUser
	}
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	h := &handler{
		db:     d.DB,
		model:  d.Model,
		movies: d.Movies,
		logger: logger,
		cache:  newLRU(o.cacheEntries, o.cacheBytes, o.cachePerUser),
	}
	session := auth.RequireSession(d)
	perSession := ratelimit.Middleware(ratelimit.NewKeyed(ratelimit.RecommendPerSession), auth.SessionKey)
	userBuckets := ratelimit.NewKeyed(ratelimit.RecommendPerSession)
	perUser := ratelimit.Middleware(userBuckets, userKey)
	if o.observe != nil {
		o.observe(h.cache, userBuckets)
	}
	limit := func(next http.Handler) http.Handler { return perSession(perUser(next)) }
	// A deleted account's cached answers and rate-limit bucket go with it
	// and can't come back, even from a request already in flight (security
	// audit SA1-1). Its sessions are deleted with the account, so the
	// per-session buckets can never be used again and simply age out.
	d.DB.OnUserDeleted(func(uid int64) {
		h.cache.retireUser(uid)
		userBuckets.Retire(userBucket(uid))
	})
	mux.Handle("GET /api/recommend", session(limit(http.HandlerFunc(h.recommend))))
	mux.Handle("GET /api/recommend/compare", session(limit(http.HandlerFunc(h.compare))))
}

// userKey is the per-user rate-limit key.
func userKey(r *http.Request) (string, bool) {
	uid, ok := auth.UserID(r.Context())
	if !ok {
		return "", false
	}
	return userBucket(uid), true
}

func userBucket(uid int64) string { return "user:" + strconv.FormatInt(uid, 10) }

type handler struct {
	db     *db.DB
	model  modelclient.Client
	movies *moviecache.Cache
	logger *slog.Logger
	cache  *lru

	defMu      sync.Mutex
	defVersion string // artifact version defModel was read for
	defModel   modelclient.ModelName
}

// userRatings is what recs-model needs about the caller.
type userRatings struct {
	version int64                  // users.ratings_version
	created int64                  // users.created_at, fixed for the account's life
	ratings []modelclient.RatingIn // newest first
	ids     []int64                // the rated movie IDs, same order (exclude list)
}

// recommend serves GET /api/recommend.
func (h *handler) recommend(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	var model modelclient.ModelName // "" = recs-model's default_model
	if vals, ok := q["model"]; ok {
		if len(vals) != 1 || !knownModel(vals[0]) {
			apperr.Write(w, errUnknownModel(strings.Join(vals, ",")))
			return
		}
		model = modelclient.ModelName(vals[0])
	}
	k, ok := intParam(q, "k", defaultK, 1, maxK)
	if !ok {
		apperr.Write(w, errBadK)
		return
	}
	f, err := parseFilter(q)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	h.serve(w, r, "recommend", []modelclient.ModelName{model}, k, f, func(e *entry) any {
		res := e.results[0]
		return RecommendResponse{Model: res.Model, ModelVersion: e.modelVersion, Items: res.Items, ColdStart: e.coldStart}
	})
}

// compare serves GET /api/recommend/compare.
func (h *handler) compare(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	models, err := parseModels(q["models"])
	if err != nil {
		apperr.Write(w, err)
		return
	}
	k, ok := intParam(q, "k", defaultK, 1, maxK)
	if !ok {
		apperr.Write(w, errBadK)
		return
	}
	f, err := parseFilter(q)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	h.serve(w, r, "compare", models, k, f, func(e *entry) any {
		// The cache key ignores model order; list results in this request's
		// order. (A cold start answer is a single pop list.)
		results := e.results
		if !e.coldStart {
			results = make([]result, 0, len(models))
			for _, m := range models {
				for _, res := range e.results {
					if res.Model == m {
						results = append(results, res)
						break
					}
				}
			}
		}
		return CompareResponse{ModelVersion: e.modelVersion, ColdStart: e.coldStart, Results: results}
	})
}

// parseModels validates compare's models parameter: one value, at most 64
// characters, 1-5 comma-separated distinct names (bad_request), each a
// known ModelName (unknown_model).
func parseModels(vals []string) ([]modelclient.ModelName, error) {
	if len(vals) != 1 || vals[0] == "" || len(vals[0]) > maxModelsParam {
		return nil, errBadModels
	}
	names := strings.Split(vals[0], ",")
	if len(names) > maxCompare {
		return nil, errBadModels
	}
	for i, n := range names {
		if n == "" || slices.Contains(names[:i], n) {
			return nil, errBadModels
		}
		for j := 0; j < len(n); j++ {
			if (n[j] < 'a' || n[j] > 'z') && n[j] != '_' {
				return nil, errBadModels
			}
		}
	}
	out := make([]modelclient.ModelName, len(names))
	for i, n := range names {
		if !knownModel(n) {
			return nil, errUnknownModel(n)
		}
		out[i] = modelclient.ModelName(n)
	}
	return out, nil
}

// parseGenres validates the optional genres parameter (public.yaml
// parameters.Genres): absent or empty means no filter (nil); otherwise one
// value of at most 200 characters, 1-19 comma-separated names, each a known
// Genre (case-sensitive) and none repeated. Anything else is bad_request.
// The names keep the caller's order. The message never echoes the input.
func parseGenres(q map[string][]string) ([]modelclient.Genre, error) {
	vals, present := q["genres"]
	if !present {
		return nil, nil
	}
	if len(vals) != 1 || len(vals[0]) > maxGenresParam {
		return nil, errBadGenres
	}
	if vals[0] == "" {
		return nil, nil
	}
	names := strings.Split(vals[0], ",")
	if len(names) > maxGenres {
		return nil, errBadGenres
	}
	out := make([]modelclient.Genre, len(names))
	for i, n := range names {
		g := modelclient.Genre(n)
		// The enum check also rules out empty names and any character
		// outside [A-Za-z-].
		if !slices.Contains(modelclient.AllGenres, g) || slices.Contains(out[:i], g) {
			return nil, errBadGenres
		}
		out[i] = g
	}
	return out, nil
}

// genresKey is the cache key's genre component: the set, sorted, so the same
// set in any order shares an entry. "" means no filter, which no non-empty
// set can produce.
func genresKey(genres []modelclient.Genre) string {
	s := make([]string, len(genres))
	for i, g := range genres {
		s[i] = string(g)
	}
	slices.Sort(s)
	return strings.Join(s, ",")
}

// filter is what a request restricts its lists to. The zero value means no
// restriction.
type filter struct {
	genres   []modelclient.Genre // nil = any genre
	yearFrom *int                // inclusive; nil = unbounded
	yearTo   *int                // inclusive; nil = unbounded
}

// parseFilter reads the optional genres, year_from and year_to parameters.
func parseFilter(q map[string][]string) (filter, error) {
	genres, err := parseGenres(q)
	if err != nil {
		return filter{}, err
	}
	from, to, err := parseYears(q)
	if err != nil {
		return filter{}, err
	}
	return filter{genres: genres, yearFrom: from, yearTo: to}, nil
}

// parseYears validates the optional year_from and year_to parameters
// (public.yaml parameters.YearFrom and YearTo): each, when present, is one
// value of ASCII digits only, a whole year in [1800, 2100]; when both are
// present year_from must not exceed year_to. An absent or empty bound is nil
// (unbounded), as an empty genres is no filter. Anything else, a repeated
// parameter included, is bad_request with one fixed message that never
// echoes the input.
func parseYears(q map[string][]string) (from, to *int, err error) {
	from, ok := yearParam(q, "year_from")
	if !ok {
		return nil, nil, errBadYears
	}
	to, ok = yearParam(q, "year_to")
	if !ok {
		return nil, nil, errBadYears
	}
	if from != nil && to != nil && *from > *to {
		return nil, nil, errBadYears
	}
	return from, to, nil
}

// yearParam parses one year parameter. An absent or empty one is (nil, true).
func yearParam(q map[string][]string, name string) (*int, bool) {
	vals, present := q[name]
	if !present {
		return nil, true
	}
	if len(vals) != 1 || len(vals[0]) > maxYearParam {
		return nil, false
	}
	if vals[0] == "" {
		return nil, true // year_from= means unbounded
	}
	for i := 0; i < len(vals[0]); i++ {
		if vals[0][i] < '0' || vals[0][i] > '9' {
			return nil, false
		}
	}
	n, err := strconv.Atoi(vals[0])
	if err != nil || n < modelclient.MinYear || n > modelclient.MaxYear {
		return nil, false
	}
	return &n, true
}

// yearsKey is the cache key's year component: "from..to" with an absent
// bound left empty ("1990..", "..1999", "1990..1999"), and "" when neither
// is set. Every distinct pair of bounds, absent ones included, gives a
// distinct string.
func yearsKey(from, to *int) string {
	if from == nil && to == nil {
		return ""
	}
	var b strings.Builder
	if from != nil {
		b.WriteString(strconv.Itoa(*from))
	}
	b.WriteString("..")
	if to != nil {
		b.WriteString(strconv.Itoa(*to))
	}
	return b.String()
}

func knownModel(s string) bool {
	return slices.Contains(modelclient.AllModels, modelclient.ModelName(s))
}

// serve is the shared path of both routes: read the caller's ratings, ask
// recs-model for its current version, answer from the cache or compute,
// enrich and cache. models holds one entry for op "recommend" ("" means the
// default model). The filter (genres and year range) goes to recs-model on
// every path, cold start included.
func (h *handler) serve(w http.ResponseWriter, r *http.Request, op string, models []modelclient.ModelName, k int, f filter, render func(*entry) any) {
	uid, ok := auth.UserID(r.Context())
	if !ok {
		apperr.Write(w, errNoUser)
		return
	}
	user, err := h.loadRatings(r.Context(), uid)
	if err != nil {
		h.internal(w, r, "read ratings", err)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), modelBudget)
	defer cancel()

	ver, _, err := h.model.Version(ctx)
	if err != nil {
		h.modelError(w, r, "version", err)
		return
	}
	version := ver.ArtifactVersion

	cold := len(user.ratings) < ColdStartBelow
	switch {
	case cold:
		// Cold start: popularity, whatever was asked for.
		op, models = "recommend", []modelclient.ModelName{modelclient.ModelPop}
	case models[0] == "":
		def, err := h.defaultModel(ctx, version)
		if err != nil {
			h.modelError(w, r, "models", err)
			return
		}
		models = []modelclient.ModelName{def}
	}

	keyModels := models
	if op == "compare" {
		keyModels = slices.Sorted(slices.Values(models))
	}
	key := cacheKey{userID: uid, userCreated: user.created, ratingsVersion: user.version, modelVersion: version, op: op, models: joinModels(keyModels), k: k, genres: genresKey(f.genres), years: yearsKey(f.yearFrom, f.yearTo)}
	e, hit := h.cache.get(key)
	if !hit {
		e, err = h.compute(ctx, version, op, models, k, f, user)
		if err != nil {
			h.modelError(w, r, op, err)
			return
		}
		e.coldStart = cold
		// Keyed on the version that actually computed the answer, which can
		// differ from the one just checked if recs-model swapped in between.
		key.modelVersion = e.modelVersion
		h.cache.put(key, e)
	}
	w.Header().Set("X-Model-Version", e.modelVersion)
	apperr.WriteJSON(w, http.StatusOK, render(e))
}

// compute asks recs-model for the lists and enriches them with titles.
// checked is the version /v1/version reported, used only if the response
// carries no X-Model-Version.
func (h *handler) compute(ctx context.Context, checked, op string, models []modelclient.ModelName, k int, f filter, user userRatings) (*entry, error) {
	var (
		raw  []rawResult
		meta modelclient.Meta
	)
	if op == "recommend" {
		resp, m, err := h.model.Recommend(ctx, modelclient.RecommendRequest{Model: models[0], Ratings: user.ratings, K: k, Exclude: user.ids,
			Genres: f.genres, YearFrom: f.yearFrom, YearTo: f.yearTo})
		if err != nil {
			return nil, err
		}
		meta = m
		raw = []rawResult{{model: models[0], items: resp.Items}}
	} else {
		resp, m, err := h.model.Compare(ctx, modelclient.CompareRequest{Models: models, Ratings: user.ratings, K: k, Exclude: user.ids,
			Genres: f.genres, YearFrom: f.yearFrom, YearTo: f.yearTo})
		if err != nil {
			return nil, err
		}
		meta = m
		// Results come back in request order; trust only requested models.
		byModel := make(map[modelclient.ModelName][]modelclient.Item, len(resp.Results))
		for _, res := range resp.Results {
			byModel[res.Model] = res.Items
		}
		for _, m := range models {
			raw = append(raw, rawResult{model: m, items: byModel[m]})
		}
	}
	results, err := h.enrich(ctx, raw)
	if err != nil {
		return nil, err
	}
	mv := meta.ModelVersion
	if mv == "" {
		mv = checked
	}
	return &entry{modelVersion: mv, results: results}, nil
}

type rawResult struct {
	model modelclient.ModelName
	items []modelclient.Item
}

// enrich adds titles and years from the movie cache. Movies recs-model
// can't describe are dropped (items and because entries alike); because is
// capped at maxBecause entries. It fails only when some movie could not be
// described because recs-model could not be asked.
func (h *handler) enrich(ctx context.Context, raw []rawResult) ([]result, error) {
	var ids []int64
	for _, res := range raw {
		for _, it := range res.items {
			ids = append(ids, it.MovieID)
			for _, b := range it.Because {
				ids = append(ids, b.MovieID)
			}
		}
	}
	got, err := h.movies.Get(ctx, ids)
	if err != nil && (len(got.Unchecked) > 0 || len(got.Movies) == 0) {
		return nil, err
	}
	out := make([]result, len(raw))
	for i, res := range raw {
		out[i] = result{Model: res.model, Items: []RecItem{}}
		for _, it := range res.items {
			m, ok := got.Movies[it.MovieID]
			if !ok {
				continue
			}
			item := RecItem{MovieID: it.MovieID, Title: m.Title, Year: m.Year, Score: it.Score, Because: []Because{}}
			for _, b := range it.Because {
				if len(item.Because) == maxBecause {
					break
				}
				if bm, ok := got.Movies[b.MovieID]; ok {
					item.Because = append(item.Because, Because{MovieID: b.MovieID, Title: bm.Title, Weight: b.Weight})
				}
			}
			out[i].Items = append(out[i].Items, item)
		}
	}
	return out, nil
}

// defaultModel returns /v1/models' default_model, remembered per artifact
// version.
func (h *handler) defaultModel(ctx context.Context, version string) (modelclient.ModelName, error) {
	h.defMu.Lock()
	if h.defVersion == version && h.defModel != "" {
		m := h.defModel
		h.defMu.Unlock()
		return m, nil
	}
	h.defMu.Unlock()
	resp, meta, err := h.model.Models(ctx)
	if err != nil {
		return "", err
	}
	def := resp.DefaultModel
	if !knownModel(string(def)) {
		def = modelclient.ModelPop
	}
	if meta.ModelVersion == version || resp.ArtifactVersion == version {
		h.defMu.Lock()
		h.defVersion, h.defModel = version, def
		h.defMu.Unlock()
	}
	return def, nil
}

// loadRatings reads the caller's ratings and ratings_version in one read
// transaction, so the version matches the ratings it describes.
func (h *handler) loadRatings(ctx context.Context, uid int64) (userRatings, error) {
	var u userRatings
	tx, err := h.db.Reader.BeginTx(ctx, nil)
	if err != nil {
		return u, err
	}
	defer func() { _ = tx.Rollback() }()
	if err := tx.QueryRowContext(ctx, "SELECT ratings_version, created_at FROM users WHERE id = ?", uid).Scan(&u.version, &u.created); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return u, errNoUser
		}
		return u, err
	}
	rows, err := tx.QueryContext(ctx,
		"SELECT movie_id, rating FROM ratings WHERE user_id = ? ORDER BY updated_at DESC, movie_id ASC LIMIT ?",
		uid, modelclient.MaxRatings)
	if err != nil {
		return u, err
	}
	defer rows.Close()
	u.ratings = []modelclient.RatingIn{}
	for rows.Next() {
		var ri modelclient.RatingIn
		if err := rows.Scan(&ri.MovieID, &ri.Rating); err != nil {
			return u, err
		}
		u.ratings = append(u.ratings, ri)
		u.ids = append(u.ids, ri.MovieID)
	}
	return u, rows.Err()
}

func joinModels(models []modelclient.ModelName) string {
	s := make([]string, len(models))
	for i, m := range models {
		s[i] = string(m)
	}
	return strings.Join(s, ",")
}

// modelError maps a recs-model (or movie cache) error to the public error.
// It never logs request bodies (they carry ratings).
func (h *handler) modelError(w http.ResponseWriter, r *http.Request, what string, err error) {
	var se *modelclient.StatusError
	var ae *apperr.Error
	switch {
	case errors.Is(err, modelclient.ErrUnavailable), errors.Is(err, context.DeadlineExceeded):
		h.logger.WarnContext(r.Context(), "recommend: recs-model unavailable", "request_id", reqid.FromContext(r.Context()), "stage", what)
		apperr.Write(w, moviecache.ErrModelUnavailable)
	case errors.As(err, &se) && se.Code == modelclient.ErrorCodeUnknownModel:
		apperr.Write(w, errModelNotLoaded)
	case errors.As(err, &ae):
		apperr.Write(w, err)
	default:
		h.internal(w, r, what, err)
	}
}

// internal logs err and writes 500.
func (h *handler) internal(w http.ResponseWriter, r *http.Request, what string, err error) {
	var ae *apperr.Error
	if errors.As(err, &ae) {
		apperr.Write(w, err)
		return
	}
	h.logger.ErrorContext(r.Context(), "recommend: "+what+" failed", "request_id", reqid.FromContext(r.Context()), "err", err)
	apperr.WriteError(w, http.StatusInternalServerError, apperr.CodeInternal, "internal error")
}

// intParam parses an optional integer query parameter: one value, ASCII
// digits only, within [lo, hi]. A missing parameter yields def.
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
