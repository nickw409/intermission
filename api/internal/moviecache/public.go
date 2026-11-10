package moviecache

import (
	"net/http"
	"strconv"

	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/modelclient"
)

// Public errors shared by the routes that use the cache (movies, ratings).
// Messages match contracts/fixtures/public.
var (
	ErrBadMovieID       = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "movieId must be a positive integer")
	ErrMovieNotFound    = apperr.New(http.StatusNotFound, apperr.CodeNotFound, "movie not found")
	ErrModelUnavailable = apperr.New(http.StatusServiceUnavailable, apperr.CodeModelUnavailable, "the recommendation service is unavailable")
)

// ParseMovieID parses a {movieId} path segment: ASCII digits only (no sign,
// space or exponent), value 1..2147483647 (MovieId in contracts/public.yaml).
func ParseMovieID(s string) (int64, error) {
	if s == "" || len(s) > 10 {
		return 0, ErrBadMovieID
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, ErrBadMovieID
		}
	}
	id, err := strconv.ParseInt(s, 10, 64)
	if err != nil || id < 1 || id > modelclient.MaxMovieID {
		return 0, ErrBadMovieID
	}
	return id, nil
}
