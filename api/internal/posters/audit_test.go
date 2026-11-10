package posters

import (
	"fmt"
	"math/rand/v2"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

func batchCalls(e *env) int {
	n := 0
	for _, c := range e.Fake.Calls() {
		if c.OperationID == fake.OpMoviesBatch {
			n++
		}
	}
	return n
}

// Audit M1: an anonymous loop over random uncached movie IDs costs at most
// the lookup limiter's burst in recs-model calls; past it, placeholders.
func TestRandomIDLoopBoundedModelCalls(t *testing.T) {
	const burst = 10
	e := newEnv(t, setup{key: testKey, lookup: ratelimit.NewGlobal(ratelimit.Config{Every: time.Hour, Burst: burst})})
	rng := rand.New(rand.NewPCG(1, 2))
	for i := 0; i < 200; i++ {
		id := 100_000 + rng.Int64N(2_000_000_000)
		if rec := e.poster(strconv.FormatInt(id, 10)); !isPlaceholder(rec) {
			t.Fatalf("%d: %d", id, rec.Code)
		}
	}
	if n := batchCalls(e); n > burst {
		t.Fatalf("%d recs-model calls for 200 random IDs, want <= %d", n, burst)
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("TMDB contacted for unknown movies")
	}
}

// Audit M1: a movie with no poster is asked about once per negative-cache
// TTL; unknown movies and movies without a TMDB id are remembered too.
func TestNegativeCache(t *testing.T) {
	clk := &testClock{t: time.Unix(1_800_000_000, 0)}
	e := newEnv(t, setup{key: testKey, now: clk.now})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"poster_path":null}`) })
	for i := 0; i < 5; i++ {
		if rec := e.poster("5"); !isPlaceholder(rec) {
			t.Fatal(rec.Code)
		}
	}
	if n := e.tmdb.apiHits.Load(); n != 1 {
		t.Fatalf("posterless movie fetched %d times within the TTL", n)
	}
	clk.advance(negativeTTL + time.Second)
	e.poster("5")
	if n := e.tmdb.apiHits.Load(); n != 2 {
		t.Fatalf("after the TTL: %d fetches, want 2", n)
	}

	// Upstream failures are remembered too.
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "x", http.StatusInternalServerError) })
	e.poster("6")
	e.poster("6")
	if n := e.tmdb.apiHits.Load(); n != 3 {
		t.Fatalf("failed fetch retried within the TTL: %d", n)
	}

	before := batchCalls(e)
	for i := 0; i < 3; i++ {
		e.poster("999999999") // unknown to recs-model
		e.poster("60")        // cached, no TMDB id
	}
	if n := batchCalls(e) - before; n != 1 {
		t.Fatalf("unknown movie looked up %d times", n)
	}
}

// Model unavailability is transient and is not remembered.
func TestModelDownNotNegativelyCached(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	e.Fake.SetDown(true)
	e.poster("4993")
	e.Fake.SetDown(false)
	e.poster("4993")
	if n := batchCalls(e); n != 2 {
		t.Fatalf("%d lookups, want 2", n)
	}
}

func TestNegativeCacheBounded(t *testing.T) {
	clk := &testClock{t: time.Unix(0, 0)}
	n := newNegCache(3, time.Minute, clk.now)
	for id := int64(1); id <= 10; id++ {
		n.add(id)
	}
	if n.len() != 3 {
		t.Fatalf("len %d", n.len())
	}
	if n.has(1) || !n.has(10) {
		t.Fatal("kept the wrong entries")
	}
	clk.advance(time.Minute + time.Second)
	if n.has(10) {
		t.Fatal("expired entry still present")
	}
}

// Audit test gap (a): poster_path values that aren't a plain "/name.jpg"
// give the placeholder without any image fetch.
func TestBadPosterPathNoImageFetch(t *testing.T) {
	for i, p := range []string{"/../../x.jpg", "/a.jpg?x", "//evil/x.jpg", "", "a.jpg", "/a.jpg#x", "/a%2e.jpg", "/a/b.jpg", "/a.png", "/" + strings.Repeat("a", 70) + ".jpg"} {
		t.Run("case"+strconv.Itoa(i), func(t *testing.T) {
			t.Logf("poster_path %q", p)
			e := newEnv(t, setup{key: testKey})
			e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprintf(w, `{"poster_path":%q}`, p) })
			if rec := e.poster("1"); !isPlaceholder(rec) {
				t.Fatalf("%d", rec.Code)
			}
			if e.tmdb.imgHits.Load() != 0 {
				t.Fatal("image host contacted")
			}
		})
	}
}

// Audit L2: concurrent gets and puts keep the index and the directory under
// the cap (run with -race).
func TestDiskCacheConcurrent(t *testing.T) {
	dir := t.TempDir()
	const capBytes = 5500
	c := newDiskCache(dir, capBytes, nil)
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 200; i++ {
				id := int64((g*7+i)%20 + 1)
				if b, ok := c.get(id); ok && !isPoster200(b, byte(id)) {
					t.Errorf("poster %d corrupted", id)
				}
				c.put(id, jpeg(1000, byte(id)))
			}
		}(g)
	}
	wg.Wait()
	if total, names := dirSize(t, dir); total > capBytes {
		t.Fatalf("%d bytes on disk (%v)", total, names)
	}
	if c.bytes() > capBytes {
		t.Fatalf("index total %d", c.bytes())
	}
}

func isPoster200(b []byte, fill byte) bool {
	return len(b) == 1000 && b[len(b)-1] == fill
}

// Info: cache files are opened without following symlinks and only if
// regular.
func TestDiskCacheRefusesSymlinkAndFIFO(t *testing.T) {
	dir := t.TempDir()
	secret := filepath.Join(t.TempDir(), "secret")
	if err := os.WriteFile(secret, jpeg(1000, 1), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(secret, filepath.Join(dir, "1.jpg")); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Mkfifo(filepath.Join(dir, "2.jpg"), 0o644); err != nil {
		t.Skip("mkfifo:", err)
	}
	c := newDiskCache(dir, 1<<20, nil)
	// Planted after the index was built, too.
	c.mu.Lock()
	c.entries[1] = c.order.PushFront(&diskItem{id: 1, size: 1000})
	c.entries[2] = c.order.PushFront(&diskItem{id: 2, size: 1000})
	c.total += 2000
	c.mu.Unlock()
	done := make(chan bool)
	go func() {
		_, ok1 := c.get(1)
		_, ok2 := c.get(2)
		done <- ok1 || ok2
	}()
	select {
	case served := <-done:
		if served {
			t.Fatal("served a symlink or FIFO")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("blocked on a FIFO")
	}
}

type testClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *testClock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *testClock) advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}
