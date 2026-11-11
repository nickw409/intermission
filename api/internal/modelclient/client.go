package modelclient

import (
	"context"
	"errors"
	"fmt"
)

// Client is how recs-api talks to recs-model. There is one method per
// operation in contracts/model.yaml, except /metrics (scraped by Prometheus,
// never called by recs-api).
//
// Every method that returns data also returns the response's X-Model-Version
// header in Meta. Errors are:
//   - ErrUnavailable (match with errors.Is) for connection errors, timeouts,
//     context deadlines and 503 responses;
//   - *StatusError (match with errors.As) for any other non-2xx response
//     from recs-model (400 bad_request/invalid_json/unknown_model/
//     body_too_large, 404 not_found).
type Client interface {
	// Recommend calls POST /v1/recommend.
	Recommend(ctx context.Context, req RecommendRequest) (RecommendResponse, Meta, error)
	// Compare calls POST /v1/compare.
	Compare(ctx context.Context, req CompareRequest) (CompareResponse, Meta, error)
	// Similar calls GET /v1/similar/{movie_id}?model=&k=. An empty model or a
	// zero k omits that query parameter so recs-model applies its default
	// (knn, 20).
	Similar(ctx context.Context, movieID int64, model ModelName, k int) (SimilarResponse, Meta, error)
	// Onboarding calls POST /v1/onboarding.
	Onboarding(ctx context.Context, req OnboardingRequest) (IDList, Meta, error)
	// Search calls GET /v1/movies/search?q=&limit=. A zero limit omits the
	// parameter so recs-model applies its default (20).
	Search(ctx context.Context, q string, limit int) (IDList, Meta, error)
	// MoviesBatch calls POST /v1/movies/batch with {"ids": ids}.
	MoviesBatch(ctx context.Context, ids []int64) (BatchResponse, Meta, error)
	// Version calls GET /v1/version. recs-api calls it before every
	// recommendation-cache lookup and keys the cache on the returned version.
	Version(ctx context.Context) (VersionResponse, Meta, error)
	// Models calls GET /v1/models.
	Models(ctx context.Context) (ModelsResponse, Meta, error)
	// Healthz calls GET /healthz. It returns nil on 200 and ErrUnavailable
	// on 503 or when recs-model cannot be reached.
	Healthz(ctx context.Context) error
}

// Meta carries response metadata common to every recs-model response.
type Meta struct {
	// ModelVersion is the X-Model-Version header (the artifact version), or
	// ModelVersionNone before an artifact is loaded.
	ModelVersion string
}

// ModelVersionNone is the X-Model-Version value before any artifact is loaded.
const ModelVersionNone = "none"

// ErrUnavailable means recs-model could not serve the request: connection
// error, timeout, or a 503 response. Public model routes answer it with
// 503 model_unavailable.
var ErrUnavailable = errors.New("model_unavailable")

// StatusError is a non-2xx, non-503 response from recs-model, decoded from
// its Error body.
type StatusError struct {
	Status  int
	Code    string // one of ErrorCode* below
	Message string
}

func (e *StatusError) Error() string {
	return fmt.Sprintf("recs-model: %d %s: %s", e.Status, e.Code, e.Message)
}

// Error codes recs-model may return (Error.error.code enum in model.yaml).
const (
	ErrorCodeBadRequest       = "bad_request"
	ErrorCodeInvalidJSON      = "invalid_json"
	ErrorCodeBodyTooLarge     = "body_too_large"
	ErrorCodeNotFound         = "not_found"
	ErrorCodeUnknownModel     = "unknown_model"
	ErrorCodeModelUnavailable = "model_unavailable"
	ErrorCodeInternal         = "internal"
)

// Limits enforced by recs-model (model.yaml). recs-api validates against the
// same numbers before calling.
const (
	MaxRatings  = 5000
	MaxK        = 100
	MaxExclude  = 10000
	MaxBatchIDs = 200
	MaxCompare  = 5
	MaxSearchQ  = 100 // characters
	MaxSearchN  = 50
	MaxMovieID  = 2147483647
	MaxGenres   = 19 // Genres maxItems: every genre at most once
)

// ModelName is a recommender name (ModelName enum in model.yaml).
type ModelName string

// Every ModelName in model.yaml.
const (
	ModelPop      ModelName = "pop"
	ModelPopCount ModelName = "pop_count"
	ModelKNN      ModelName = "knn"
	ModelContent  ModelName = "content"
	ModelMF       ModelName = "mf"
	ModelALS      ModelName = "als"
	ModelHybrid   ModelName = "hybrid" // reserved (phase 6); 400 unknown_model until loaded
	ModelMMR      ModelName = "mmr"    // reserved (phase 6); 400 unknown_model until loaded
)

// AllModels lists every ModelName in contract order.
var AllModels = []ModelName{ModelPop, ModelPopCount, ModelKNN, ModelContent, ModelMF, ModelALS, ModelHybrid, ModelMMR}

// Genre is a MovieLens genre name (Genre enum in model.yaml; public.yaml's
// Genre enum is the same list). Names are case-sensitive.
type Genre string

// Every Genre in model.yaml.
const (
	GenreAction      Genre = "Action"
	GenreAdventure   Genre = "Adventure"
	GenreAnimation   Genre = "Animation"
	GenreChildren    Genre = "Children"
	GenreComedy      Genre = "Comedy"
	GenreCrime       Genre = "Crime"
	GenreDocumentary Genre = "Documentary"
	GenreDrama       Genre = "Drama"
	GenreFantasy     Genre = "Fantasy"
	GenreFilmNoir    Genre = "Film-Noir"
	GenreHorror      Genre = "Horror"
	GenreIMAX        Genre = "IMAX"
	GenreMusical     Genre = "Musical"
	GenreMystery     Genre = "Mystery"
	GenreRomance     Genre = "Romance"
	GenreSciFi       Genre = "Sci-Fi"
	GenreThriller    Genre = "Thriller"
	GenreWar         Genre = "War"
	GenreWestern     Genre = "Western"
)

// AllGenres lists every Genre in contract order.
var AllGenres = []Genre{GenreAction, GenreAdventure, GenreAnimation, GenreChildren, GenreComedy, GenreCrime,
	GenreDocumentary, GenreDrama, GenreFantasy, GenreFilmNoir, GenreHorror, GenreIMAX, GenreMusical, GenreMystery,
	GenreRomance, GenreSciFi, GenreThriller, GenreWar, GenreWestern}

// ErrorBody is the Error schema: {"error":{"code","message"}}.
type ErrorBody struct {
	Error ErrorDetail `json:"error"`
}

// ErrorDetail is the inner object of ErrorBody.
type ErrorDetail struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// RatingIn is one rating sent to recs-model. Rating is in [0.5, 5] in 0.5 steps.
type RatingIn struct {
	MovieID int64   `json:"movie_id"`
	Rating  float64 `json:"rating"`
}

// RecommendRequest is the body of POST /v1/recommend.
type RecommendRequest struct {
	Model   ModelName  `json:"model"`
	Ratings []RatingIn `json:"ratings"`
	K       int        `json:"k"`
	Exclude []int64    `json:"exclude,omitempty"`
	// Genres restricts results to movies with at least one of these genres.
	// Empty means no restriction (the field is then omitted).
	Genres []Genre `json:"genres,omitempty"`
}

// CompareRequest is the body of POST /v1/compare.
type CompareRequest struct {
	Models  []ModelName `json:"models"`
	Ratings []RatingIn  `json:"ratings"`
	K       int         `json:"k"`
	Exclude []int64     `json:"exclude,omitempty"`
	// Genres: as in RecommendRequest.
	Genres []Genre `json:"genres,omitempty"`
}

// Because is one explanation entry: a movie the user rated and its weight.
type Because struct {
	MovieID int64   `json:"movie_id"`
	Weight  float64 `json:"weight"`
}

// Item is one recommended movie.
type Item struct {
	MovieID int64     `json:"movie_id"`
	Score   float64   `json:"score"`
	Because []Because `json:"because"`
}

// RecommendResponse is the 200 body of POST /v1/recommend.
type RecommendResponse struct {
	Model   ModelName `json:"model"`
	Items   []Item    `json:"items"`
	Ignored int       `json:"ignored"`
}

// CompareResult is one model's list in a CompareResponse.
type CompareResult struct {
	Model ModelName `json:"model"`
	Items []Item    `json:"items"`
}

// CompareResponse is the 200 body of POST /v1/compare (results in request order).
type CompareResponse struct {
	Results []CompareResult `json:"results"`
	Ignored int             `json:"ignored"`
}

// SimilarItem is one neighbour in a SimilarResponse.
type SimilarItem struct {
	MovieID    int64   `json:"movie_id"`
	Similarity float64 `json:"similarity"`
}

// SimilarResponse is the 200 body of GET /v1/similar/{movie_id}. Model is
// knn or content.
type SimilarResponse struct {
	Model   ModelName     `json:"model"`
	MovieID int64         `json:"movie_id"`
	Items   []SimilarItem `json:"items"`
}

// OnboardingRequest is the body of POST /v1/onboarding.
type OnboardingRequest struct {
	N       int     `json:"n"`
	Exclude []int64 `json:"exclude,omitempty"`
}

// IDList is the 200 body of /v1/onboarding and /v1/movies/search.
type IDList struct {
	MovieIDs []int64 `json:"movie_ids"`
}

// BatchRequest is the body of POST /v1/movies/batch.
type BatchRequest struct {
	IDs []int64 `json:"ids"`
}

// MovieMeta is catalog metadata for one movie. Year, TMDBID and MeanRating
// are nullable in the contract.
type MovieMeta struct {
	MovieID     int64    `json:"movie_id"`
	Title       string   `json:"title"`
	Year        *int     `json:"year"`
	Genres      []string `json:"genres"`
	TMDBID      *int64   `json:"tmdb_id"`
	RatingCount int      `json:"rating_count"`
	MeanRating  *float64 `json:"mean_rating"`
}

// BatchResponse is the 200 body of POST /v1/movies/batch: known movies in
// request order, unknown IDs in Missing.
type BatchResponse struct {
	Movies  []MovieMeta `json:"movies"`
	Missing []int64     `json:"missing"`
}

// ModelsResponse is the 200 body of GET /v1/models. TrainedAt is an RFC 3339
// date-time, kept as the string recs-model sent.
type ModelsResponse struct {
	ArtifactVersion string      `json:"artifact_version"`
	TrainedAt       string      `json:"trained_at"`
	DefaultModel    ModelName   `json:"default_model"`
	Models          []ModelName `json:"models"`
}

// VersionResponse is the 200 body of GET /v1/version (Version schema).
type VersionResponse struct {
	ArtifactVersion string `json:"artifact_version"`
}

// Health is the body of GET /healthz (status "ok" or "unavailable").
type Health struct {
	Status string `json:"status"`
}
