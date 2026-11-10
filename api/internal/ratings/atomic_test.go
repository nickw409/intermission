package ratings

import (
	"context"
	"database/sql"
	"errors"
	"testing"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// failingBump performs the real ratings_version bump and then fails, so any
// write that is not in the same transaction as the bump survives and is
// caught below (whether it was committed before or after the bump).
func failingBump(t *testing.T) *int {
	t.Helper()
	calls := 0
	bump = func(ctx context.Context, tx *sql.Tx, uid int64) error {
		calls++
		if err := bumpVersion(ctx, tx, uid); err != nil {
			return err
		}
		return errors.New("injected failure after the bump")
	}
	t.Cleanup(func() { bump = bumpVersion })
	return &calls
}

// The rating upsert and the ratings_version bump commit together or not at
// all.
func TestPutIsAtomicWithVersionBump(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000), cachetest.Movie(11, "Eleven", 2000))
	seedRatings(t, e.DB, uid, ratingRow{11, 2, 7})
	calls := failingBump(t)

	if rec := e.Do("PUT", "/api/ratings/10", map[string]any{"rating": 4}, tok); rec.Code != 500 {
		t.Fatalf("new rating: %d %s", rec.Code, rec.Body.String())
	}
	if rec := e.Do("PUT", "/api/ratings/11", map[string]any{"rating": 5}, tok); rec.Code != 500 {
		t.Fatalf("update: %d %s", rec.Code, rec.Body.String())
	}
	if *calls != 2 {
		t.Fatalf("bump called %d times", *calls)
	}
	var n int
	var r float64
	var ts int64
	_ = e.DB.Reader.QueryRow("SELECT count(*) FROM ratings WHERE user_id = ? AND movie_id = 10", uid).Scan(&n)
	_ = e.DB.Reader.QueryRow("SELECT rating, updated_at FROM ratings WHERE user_id = ? AND movie_id = 11", uid).Scan(&r, &ts)
	if n != 0 || r != 2 || ts != 7 || cachetest.RatingsVersion(t, e.DB, uid) != 0 {
		t.Fatalf("partial write survived: new rows %d, old rating %v@%d, version %d", n, r, ts, cachetest.RatingsVersion(t, e.DB, uid))
	}
}

func TestDeleteIsAtomicWithVersionBump(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedRatings(t, e.DB, uid, ratingRow{10, 3, 1})
	calls := failingBump(t)
	if rec := e.Do("DELETE", "/api/ratings/10", nil, tok); rec.Code != 500 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if *calls != 1 || countRatings(t, e.DB, uid) != 1 || cachetest.RatingsVersion(t, e.DB, uid) != 0 {
		t.Fatalf("calls %d, rows %d, version %d", *calls, countRatings(t, e.DB, uid), cachetest.RatingsVersion(t, e.DB, uid))
	}
}
