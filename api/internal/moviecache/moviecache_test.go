package moviecache_test

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"slices"
	"sync"
	"testing"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

func newFake(t *testing.T) *fake.Fake {
	t.Helper()
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func batchCalls(f *fake.Fake) [][]int64 {
	var out [][]int64
	for _, c := range f.Calls() {
		if c.OperationID != fake.OpMoviesBatch {
			continue
		}
		var b modelclient.BatchRequest
		_ = json.Unmarshal(c.Body, &b)
		out = append(out, b.IDs)
	}
	return out
}

func TestGetFillsAndThenServesFromCache(t *testing.T) {
	d := cachetest.OpenDB(t)
	f := newFake(t)
	c := moviecache.New(d, f)
	ctx := context.Background()

	res, err := c.Get(ctx, []int64{260, 1196, 1210})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Movies) != 3 || len(res.Unknown) != 0 || len(res.Unchecked) != 0 {
		t.Fatalf("res = %+v", res)
	}
	m := res.Movies[260]
	if m.Title != "Star Wars: Episode IV - A New Hope" || *m.Year != 1977 || *m.TMDBID != 11 || m.RatingCount != 251 ||
		*m.MeanRating != 4.23 || !slices.Equal(m.Genres, []string{"Action", "Adventure", "Sci-Fi"}) {
		t.Fatalf("260 = %+v", m)
	}

	// Second time: no model call at all, even with the model down.
	f.ResetCalls()
	f.SetDown(true)
	res2, err := c.Get(ctx, []int64{1210, 260, 1196})
	if err != nil {
		t.Fatal(err)
	}
	if len(f.Calls()) != 0 {
		t.Fatalf("cached Get called recs-model: %+v", f.Calls())
	}
	if !reflect.DeepEqual(res.Movies, res2.Movies) {
		t.Fatalf("cached rows differ:\n%+v\n%+v", res.Movies, res2.Movies)
	}
}

func TestGetReportsUnknown(t *testing.T) {
	d := cachetest.OpenDB(t)
	f := newFake(t)
	c := moviecache.New(d, f)
	res, err := c.Get(context.Background(), []int64{4993, 999999999})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Movies) != 1 || res.Movies[4993].Title == "" || !slices.Equal(res.Unknown, []int64{999999999}) {
		t.Fatalf("res = %+v", res)
	}
	// Unknown IDs are not cached as negatives: they are asked again.
	f.ResetCalls()
	if _, err := c.Get(context.Background(), []int64{999999999}); err != nil {
		t.Fatal(err)
	}
	if got := batchCalls(f); len(got) != 1 {
		t.Fatalf("batch calls = %v", got)
	}
}

// Misses go to recs-model deduplicated, in first-seen order.
func TestGetRequestsMissesInOrder(t *testing.T) {
	d := cachetest.OpenDB(t)
	cachetest.SeedMovies(t, d, cachetest.Movie(1196, "Empire", 1980))
	f := newFake(t)
	c := moviecache.New(d, f)
	if _, err := c.Get(context.Background(), []int64{4993, 1196, 4993}); err != nil {
		t.Fatal(err)
	}
	if got := batchCalls(f); !reflect.DeepEqual(got, [][]int64{{4993}}) {
		t.Fatalf("batch calls = %v", got)
	}
}

func TestModelDownReturnsCachedAndReportsMisses(t *testing.T) {
	d := cachetest.OpenDB(t)
	cachetest.SeedMovies(t, d, cachetest.Movie(260, "Star Wars", 1977))
	f := newFake(t)
	f.SetDown(true)
	c := moviecache.New(d, f)
	res, err := c.Get(context.Background(), []int64{260, 4993})
	if !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("err = %v", err)
	}
	if res.Movies[260].Title != "Star Wars" || !slices.Equal(res.Unchecked, []int64{4993}) || len(res.Unknown) != 0 {
		t.Fatalf("res = %+v", res)
	}
}

func TestEmptyAndAllCached(t *testing.T) {
	d := cachetest.OpenDB(t)
	f := newFake(t)
	c := moviecache.New(d, f)
	res, err := c.Get(context.Background(), nil)
	if err != nil || len(res.Movies) != 0 || len(f.Calls()) != 0 {
		t.Fatalf("empty: %+v %v", res, err)
	}
}

// stubModel answers MoviesBatch with a synthetic movie for every ID < 1e6
// and records batch sizes. It also returns a movie nobody asked for.
type stubModel struct {
	modelclient.Client
	mu    sync.Mutex
	sizes []int
	fail  bool
}

func (s *stubModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	s.mu.Lock()
	s.sizes = append(s.sizes, len(ids))
	fail := s.fail
	s.mu.Unlock()
	if fail {
		return modelclient.BatchResponse{}, modelclient.Meta{}, modelclient.ErrUnavailable
	}
	resp := modelclient.BatchResponse{Movies: []modelclient.MovieMeta{cachetest.Movie(777777777, "Unrequested", 2000)}, Missing: []int64{}}
	for _, id := range ids {
		if id < 1_000_000 {
			resp.Movies = append(resp.Movies, cachetest.Movie(id, "M", 2000))
		} else {
			resp.Missing = append(resp.Missing, id)
		}
	}
	return resp, modelclient.Meta{ModelVersion: "v"}, nil
}

func TestBatchesOfAtMost200(t *testing.T) {
	d := cachetest.OpenDB(t)
	m := &stubModel{}
	c := moviecache.New(d, m)
	ids := make([]int64, 0, 450)
	for i := int64(1); i <= 450; i++ {
		ids = append(ids, i)
	}
	res, err := c.Get(context.Background(), ids)
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Movies) != 450 || !slices.Equal(m.sizes, []int{200, 200, 50}) {
		t.Fatalf("movies %d sizes %v", len(res.Movies), m.sizes)
	}
	// Unrequested movies in a response are ignored, not cached.
	var n int
	if err := d.Reader.QueryRow("SELECT count(*) FROM movies WHERE movie_id = 777777777").Scan(&n); err != nil || n != 0 {
		t.Fatalf("unrequested movie cached: %d %v", n, err)
	}
	if _, ok := res.Movies[777777777]; ok {
		t.Fatal("unrequested movie returned")
	}
}

// After the first failed batch the rest are not attempted (each would wait
// for the same dead model) and are reported unchecked.
func TestStopsAfterFirstFailedBatch(t *testing.T) {
	d := cachetest.OpenDB(t)
	m := &stubModel{fail: true}
	c := moviecache.New(d, m)
	ids := make([]int64, 0, 450)
	for i := int64(1); i <= 450; i++ {
		ids = append(ids, i)
	}
	res, err := c.Get(context.Background(), ids)
	if !errors.Is(err, modelclient.ErrUnavailable) || len(res.Unchecked) != 450 || len(m.sizes) != 1 {
		t.Fatalf("err %v unchecked %d calls %v", err, len(res.Unchecked), m.sizes)
	}
}

func TestNullableFieldsRoundTrip(t *testing.T) {
	d := cachetest.OpenDB(t)
	c := moviecache.New(d, &stubModel{})
	in := modelclient.MovieMeta{MovieID: 5, Title: "No year", Genres: []string{}, RatingCount: 0}
	if err := c.Put(context.Background(), []modelclient.MovieMeta{in}); err != nil {
		t.Fatal(err)
	}
	res, err := c.Lookup(context.Background(), []int64{5})
	if err != nil {
		t.Fatal(err)
	}
	got := res[5]
	if got.Year != nil || got.TMDBID != nil || got.MeanRating != nil || got.Genres == nil || len(got.Genres) != 0 || got.Title != "No year" {
		t.Fatalf("got %+v", got)
	}
}

// Refresh always asks recs-model (fresh counts for the movie page) and falls
// back to the cached row when it is down.
func TestRefresh(t *testing.T) {
	d := cachetest.OpenDB(t)
	old := cachetest.Movie(4993, "LOTR", 2001)
	old.RatingCount = 3
	cachetest.SeedMovies(t, d, old)
	f := newFake(t)
	c := moviecache.New(d, f)
	ctx := context.Background()

	m, ok, err := c.Refresh(ctx, 4993)
	if err != nil || !ok || m.RatingCount != 198 || m.Title != "Lord of the Rings: The Fellowship of the Ring, The" {
		t.Fatalf("refresh: %+v %v %v", m, ok, err)
	}
	cached, _ := c.Lookup(ctx, []int64{4993})
	if cached[4993].RatingCount != 198 {
		t.Fatalf("refresh did not update the row: %+v", cached[4993])
	}

	f.SetDown(true)
	if m, ok, err := c.Refresh(ctx, 4993); err != nil || !ok || m.RatingCount != 198 {
		t.Fatalf("refresh with model down, cached: %+v %v %v", m, ok, err)
	}
	if _, _, err := c.Refresh(ctx, 2571); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("refresh with model down, uncached: %v", err)
	}
	f.SetDown(false)
	if _, ok, err := c.Refresh(ctx, 999999999); err != nil || ok {
		t.Fatalf("refresh unknown: %v %v", ok, err)
	}
}

// Concurrent fills of overlapping IDs must not fail (single writer, upserts).
func TestConcurrentGet(t *testing.T) {
	d := cachetest.OpenDB(t)
	c := moviecache.New(d, &stubModel{})
	var wg sync.WaitGroup
	errs := make(chan error, 16)
	for g := 0; g < 16; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			ids := []int64{1, 2, 3, int64(10 + g)}
			res, err := c.Get(context.Background(), ids)
			if err == nil && len(res.Movies) != 4 {
				err = errors.New("short result")
			}
			errs <- err
		}(g)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
}
