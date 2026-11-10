package moviecache_test

import (
	"context"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// Audit M1: Refresh asks recs-model only when the row is older than the
// TTL, and writes only when the metadata changed.
func TestRefreshTTL(t *testing.T) {
	if moviecache.DefaultRefreshTTL != 10*time.Minute {
		t.Fatalf("DefaultRefreshTTL = %v", moviecache.DefaultRefreshTTL)
	}
	d := cachetest.OpenDB(t)
	writes := cachetest.CountMovieWrites(t, d)
	f := newFake(t)
	var mu sync.Mutex
	now := time.Unix(1794259200, 0)
	clock := func() time.Time { mu.Lock(); defer mu.Unlock(); return now }
	advance := func(d time.Duration) { mu.Lock(); now = now.Add(d); mu.Unlock() }
	c := moviecache.New(d, f, moviecache.WithClock(clock))
	ctx := context.Background()
	calls := func() int { return len(batchCalls(f)) }

	if m, ok, err := c.Refresh(ctx, 4993); err != nil || !ok || m.RatingCount != 198 {
		t.Fatalf("first refresh: %+v %v %v", m, ok, err)
	}
	if calls() != 1 || writes() != 1 {
		t.Fatalf("first refresh: %d calls, %d writes", calls(), writes())
	}
	for i := 0; i < 5; i++ {
		if m, ok, err := c.Refresh(ctx, 4993); err != nil || !ok || m.RatingCount != 198 {
			t.Fatalf("cached refresh: %+v %v %v", m, ok, err)
		}
	}
	if calls() != 1 || writes() != 1 {
		t.Fatalf("within TTL: %d calls, %d writes", calls(), writes())
	}

	// Past the TTL: one model call; the data is unchanged, so no write, and
	// the check counts as fresh for another TTL.
	advance(moviecache.DefaultRefreshTTL + time.Second)
	for i := 0; i < 3; i++ {
		if _, _, err := c.Refresh(ctx, 4993); err != nil {
			t.Fatal(err)
		}
	}
	if calls() != 2 || writes() != 1 {
		t.Fatalf("after TTL, unchanged: %d calls, %d writes", calls(), writes())
	}

	// Changed metadata is written once.
	cachetest.SeedMovies(t, d, cachetest.Movie(5, "Old title", 1999)) // updated_at 0: stale
	base := writes()
	m := &stubModel{}
	c2 := moviecache.New(d, m, moviecache.WithClock(clock))
	if got, ok, err := c2.Refresh(ctx, 5); err != nil || !ok || got.Title != "M" {
		t.Fatalf("changed: %+v %v %v", got, ok, err)
	}
	if writes() != base+1 || len(m.sizes) != 1 {
		t.Fatalf("changed: %d writes, %d calls", writes()-base, len(m.sizes))
	}
	if _, _, err := c2.Refresh(ctx, 5); err != nil || len(m.sizes) != 1 || writes() != base+1 {
		t.Fatalf("changed then cached: %d calls %d writes %v", len(m.sizes), writes()-base, err)
	}
}

func TestWithRefreshTTLZeroAlwaysAsks(t *testing.T) {
	d := cachetest.OpenDB(t)
	f := newFake(t)
	c := moviecache.New(d, f, moviecache.WithRefreshTTL(0))
	for i := 0; i < 3; i++ {
		if _, _, err := c.Refresh(context.Background(), 4993); err != nil {
			t.Fatal(err)
		}
	}
	if n := len(batchCalls(f)); n != 3 {
		t.Fatalf("calls = %d", n)
	}
}
