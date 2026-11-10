package movies

import (
	"net/http"
	"strconv"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// Audit M1: repeated anonymous movie-page views within the refresh TTL make
// no recs-model calls and no database writes.
func TestGetMovieWithinTTLNoCallsNoWrites(t *testing.T) {
	e := newEnv(t)
	writes := cachetest.CountMovieWrites(t, e.DB)
	if rec := e.Do("GET", "/api/movies/4993", nil, ""); rec.Code != 200 {
		t.Fatalf("first: %d", rec.Code)
	}
	if writes() != 1 || len(e.Fake.Calls()) != 1 {
		t.Fatalf("first view: %d writes, %d calls", writes(), len(e.Fake.Calls()))
	}
	e.Fake.ResetCalls()
	for i := 0; i < 20; i++ {
		if rec := e.Do("GET", "/api/movies/4993", nil, ""); rec.Code != 200 {
			t.Fatalf("view %d: %d", i, rec.Code)
		}
	}
	if writes() != 1 || len(e.Fake.Calls()) != 0 {
		t.Fatalf("repeat views: %d writes, %d model calls %+v", writes()-1, len(e.Fake.Calls()), e.Fake.Calls())
	}
}

// Audit M2: the public catalog routes share one global token bucket; when
// it is empty they answer 429 rate_limited with Retry-After. Session routes
// (onboarding) are not charged.
func TestCatalogGlobalLimit(t *testing.T) {
	budget := ratelimit.Config{Every: time.Hour, Burst: 4}
	e := cachetest.NewEnv(t, nil, func(mux *http.ServeMux, d *app.Deps) { register(mux, d, budget) })
	_, tok := cachetest.NewUser(t, e.DB, "alice")
	for _, path := range []string{"/api/movies/search?q=star&limit=3", "/api/movies/4993", "/api/movies/260/similar?model=knn&k=3", "/api/models"} {
		if rec := e.Do("GET", path, nil, ""); rec.Code != 200 {
			t.Fatalf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
	}
	calls := len(e.Fake.Calls())
	for _, path := range []string{"/api/movies/search?q=star", "/api/movies/4993", "/api/movies/260/similar", "/api/models"} {
		rec := e.Do("GET", path, nil, "")
		if rec.Code != http.StatusTooManyRequests || cachetest.ErrorCode(rec) != "rate_limited" {
			t.Fatalf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
		if n, err := strconv.Atoi(rec.Header().Get("Retry-After")); err != nil || n < 1 {
			t.Fatalf("%s: Retry-After %q", path, rec.Header().Get("Retry-After"))
		}
	}
	if len(e.Fake.Calls()) != calls {
		t.Fatal("limited requests reached recs-model")
	}
	if rec := e.Do("GET", "/api/onboarding?n=4", nil, tok); rec.Code != 200 {
		t.Fatalf("onboarding charged to the catalog budget: %d", rec.Code)
	}
}

func TestCatalogBudget(t *testing.T) {
	// 20 requests per second sustained, bursts of 100.
	if CatalogGlobal.Every != 50*time.Millisecond || CatalogGlobal.Burst != 100 {
		t.Fatalf("CatalogGlobal = %+v", CatalogGlobal)
	}
}
