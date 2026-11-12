package posters

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

const testKey = "test-tmdb-key-not-real"

// jpeg returns a fake JPEG of n bytes (correct magic, filler body).
func jpeg(n int, fill byte) []byte {
	b := bytes.Repeat([]byte{fill}, n)
	copy(b, []byte{0xFF, 0xD8, 0xFF, 0xE0})
	return b
}

// tmdb is a fake TMDB: an API server and an image server on different
// hosts. Handlers can be swapped per test.
type tmdb struct {
	api, img     *httptest.Server
	apiHits      atomic.Int64
	imgHits      atomic.Int64
	mu           sync.Mutex
	apiHandler   http.HandlerFunc
	imgHandler   http.HandlerFunc
	sawKey       atomic.Bool
	lastAPIQuery string
}

func newTMDB(t *testing.T) *tmdb {
	t.Helper()
	f := &tmdb{}
	f.apiHandler = func(w http.ResponseWriter, r *http.Request) {
		// /3/movie/{tmdbId}
		id := strings.TrimPrefix(r.URL.Path, "/3/movie/")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"id":%s,"title":"x","poster_path":"/p%s.jpg"}`, id, id)
	}
	f.imgHandler = func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/t/p/w342/p") {
			http.NotFound(w, r)
			return
		}
		n, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/t/p/w342/p"), ".jpg"))
		w.Header().Set("Content-Type", "image/jpeg")
		_, _ = w.Write(jpeg(1000, byte(n-1000))) // TMDB id 1000+movieId: fill = movieId
	}
	f.api = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.apiHits.Add(1)
		if r.URL.Query().Get("api_key") == testKey {
			f.sawKey.Store(true)
		}
		f.mu.Lock()
		h := f.apiHandler
		f.lastAPIQuery = r.URL.RawQuery
		f.mu.Unlock()
		h(w, r)
	}))
	f.img = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.imgHits.Add(1)
		f.mu.Lock()
		h := f.imgHandler
		f.mu.Unlock()
		h(w, r)
	}))
	t.Cleanup(f.api.Close)
	t.Cleanup(f.img.Close)
	return f
}

func (f *tmdb) setAPI(h http.HandlerFunc) { f.mu.Lock(); f.apiHandler = h; f.mu.Unlock() }
func (f *tmdb) setImg(h http.HandlerFunc) { f.mu.Lock(); f.imgHandler = h; f.mu.Unlock() }

type env struct {
	*cachetest.Env
	tmdb *tmdb
	dir  string
}

type setup struct {
	key      string
	maxBytes int64
	limiter  *ratelimit.Global // upstream (TMDB) limiter
	lookup   *ratelimit.Global // per-route limiter before the movie lookup
	timeout  time.Duration
	now      func() time.Time
	dir      string // poster cache directory; default a new temporary one
}

func newEnv(t *testing.T, s setup) *env {
	t.Helper()
	f := newTMDB(t)
	dir := s.dir
	if dir == "" {
		dir = filepath.Join(t.TempDir(), "posters")
	}
	if s.maxBytes == 0 {
		s.maxBytes = 1 << 20
	}
	if s.limiter == nil {
		s.limiter = ratelimit.NewGlobal(ratelimit.Config{Every: time.Millisecond, Burst: 1000})
	}
	e := cachetest.NewEnv(t, nil, func(mux *http.ServeMux, d *app.Deps) {
		d.Config.TMDBAPIKey = app.Secret(s.key)
		d.Config.PosterCacheDir = dir
		d.Config.PosterCacheMaxByte = s.maxBytes
		register(mux, d, options{
			apiBase:   f.api.URL,
			imageBase: f.img.URL,
			limiter:   s.limiter,
			lookup:    s.lookup,
			timeout:   s.timeout,
			now:       s.now,
		})
	})
	// Movies 1..50 have TMDB id 1000+id; 60 has none.
	for id := int64(1); id <= 50; id++ {
		m := cachetest.Movie(id, "M", 2000)
		tm := 1000 + id
		m.TMDBID = &tm
		cachetest.SeedMovies(t, e.DB, m)
	}
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(60, "No TMDB", 2000))
	return &env{Env: e, tmdb: f, dir: dir}
}

func (e *env) poster(id string) *httptest.ResponseRecorder {
	return e.Do("GET", "/api/posters/"+id, nil, "")
}

func isPlaceholder(rec *httptest.ResponseRecorder) bool {
	return rec.Code == 200 && rec.Header().Get("Content-Type") == "image/svg+xml" && rec.Body.String() == placeholderSVG
}

func isPoster(rec *httptest.ResponseRecorder, fill byte) bool {
	return rec.Code == 200 && rec.Header().Get("Content-Type") == "image/jpeg" && bytes.Equal(rec.Body.Bytes(), jpeg(1000, fill))
}

// ---------------------------------------------------------------- contract

func TestContractFixtures(t *testing.T) {
	for _, c := range cachetest.PublicCases(t, "getPoster") {
		t.Run(c.Name, func(t *testing.T) {
			e := newEnv(t, setup{}) // no TMDB key: the placeholder
			rec := e.poster(strings.TrimPrefix(c.Request.Path, "/api/posters/"))
			if rec.Code != c.Response.Status {
				t.Fatalf("status %d, want %d", rec.Code, c.Response.Status)
			}
			for k, v := range c.Response.Headers {
				if got := rec.Header().Get(k); got != v {
					t.Errorf("%s = %q, want %q", k, got, v)
				}
			}
			var wantStr string
			if json.Unmarshal(c.Response.Body, &wantStr) == nil {
				if rec.Body.String() != wantStr {
					t.Errorf("body %q, want %q", rec.Body.String(), wantStr)
				}
				return
			}
			var got, want any
			if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
				t.Fatalf("body %q", rec.Body.String())
			}
			_ = json.Unmarshal(c.Response.Body, &want)
			g, _ := json.Marshal(got)
			w, _ := json.Marshal(want)
			if !bytes.Equal(g, w) {
				t.Errorf("body %s, want %s", g, w)
			}
		})
	}
	if e := newEnv(t, setup{}); e.tmdb.apiHits.Load() != 0 {
		t.Fatal("TMDB contacted without a key")
	}
}

func TestBadMovieIDs(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	for _, id := range []string{"abc", "0", "-1", "1.5", "2147483648", "1e3", "%2e%2e", "..%2f1"} {
		if rec := e.poster(id); rec.Code != 400 && rec.Code != 404 {
			t.Errorf("%s: %d", id, rec.Code)
		}
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("bad IDs reached TMDB")
	}
}

// ---------------------------------------------------------------- success

func TestFetchAndCache(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	rec := e.poster("7")
	if !isPoster(rec, 7) {
		t.Fatalf("%d %s %q", rec.Code, rec.Header().Get("Content-Type"), rec.Body.String())
	}
	if !e.tmdb.sawKey.Load() {
		t.Fatal("key not sent to TMDB")
	}
	if _, err := os.Stat(filepath.Join(e.dir, "7.jpg")); err != nil {
		t.Fatalf("not cached on disk: %v", err)
	}
	api, img := e.tmdb.apiHits.Load(), e.tmdb.imgHits.Load()
	if rec := e.poster("7"); !isPoster(rec, 7) {
		t.Fatal("second request")
	}
	if e.tmdb.apiHits.Load() != api || e.tmdb.imgHits.Load() != img {
		t.Fatal("cached poster fetched again")
	}
	// With TMDB failing, the poster still comes from disk.
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "down", 500) })
	if rec := e.poster("7"); !isPoster(rec, 7) {
		t.Fatal("served from disk")
	}
}

// ---------------------------------------------------------------- failures

// Gate: every upstream failure is a 200 placeholder, never a 5xx, and
// nothing is cached.
func TestFailuresGivePlaceholder(t *testing.T) {
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "image/jpeg")
		_, _ = w.Write(jpeg(1000, 9))
	}))
	defer other.Close()
	var otherHits atomic.Int64
	otherCounting := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		otherHits.Add(1)
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"poster_path":"/p1.jpg"}`)
	}))
	defer otherCounting.Close()

	huge := jpeg(MaxImageBytes+10, 1)
	cases := []struct {
		name string
		api  http.HandlerFunc
		img  http.HandlerFunc
	}{
		{"api 404", func(w http.ResponseWriter, r *http.Request) { http.NotFound(w, r) }, nil},
		{"api 500", func(w http.ResponseWriter, r *http.Request) { http.Error(w, "boom "+r.URL.RawQuery, 500) }, nil},
		{"api 401 echoing key", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, `{"status_message":"Invalid API key: `+r.URL.Query().Get("api_key")+`"}`, http.StatusUnauthorized)
		}, nil},
		{"api not json", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "<html>") }, nil},
		{"api null poster", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"poster_path":null}`) }, nil},
		{"api traversal poster path", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"poster_path":"/../../x.jpg"}`) }, nil},
		{"api absolute poster url", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprintf(w, `{"poster_path":%q}`, other.URL+"/x.jpg")
		}, nil},
		{"api huge", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `{"poster_path":"/p1.jpg","pad":"`)
			_, _ = w.Write(bytes.Repeat([]byte("a"), 2<<20))
			fmt.Fprint(w, `"}`)
		}, nil},
		{"api redirect to other host", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, otherCounting.URL+r.URL.Path, http.StatusFound)
		}, nil},
		{"api slow", func(w http.ResponseWriter, r *http.Request) {
			select {
			case <-time.After(3 * time.Second):
			case <-r.Context().Done():
			}
		}, nil},
		{"img 404", nil, func(w http.ResponseWriter, r *http.Request) { http.NotFound(w, r) }},
		{"img huge with length", nil, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "image/jpeg")
			w.Header().Set("Content-Length", strconv.Itoa(len(huge)))
			_, _ = w.Write(huge)
		}},
		{"img huge chunked", nil, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "image/jpeg")
			for i := 0; i < len(huge); i += 64 << 10 {
				_, _ = w.Write(huge[i:min(i+64<<10, len(huge))])
				w.(http.Flusher).Flush()
			}
		}},
		{"img not jpeg", nil, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "image/jpeg")
			fmt.Fprint(w, "<svg onload=alert(1)></svg>")
		}},
		{"img empty", nil, func(w http.ResponseWriter, r *http.Request) {}},
		{"img redirect to other host", nil, func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, other.URL+"/x.jpg", http.StatusFound)
		}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			e := newEnv(t, setup{key: testKey, timeout: time.Second})
			if c.api != nil {
				e.tmdb.setAPI(c.api)
			}
			if c.img != nil {
				e.tmdb.setImg(c.img)
			}
			rec := e.poster("1")
			if !isPlaceholder(rec) {
				t.Fatalf("%d %s %q", rec.Code, rec.Header().Get("Content-Type"), rec.Body.String()[:min(80, rec.Body.Len())])
			}
			if ents := posterEntries(t, e.dir); len(ents) != 0 {
				t.Fatalf("cached after failure: %v", ents)
			}
			checkNoKey(t, e, rec)
		})
	}
	if otherHits.Load() != 0 {
		t.Fatal("followed a redirect to another host")
	}
}

func TestTMDBDown(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	e.tmdb.api.Close()
	e.tmdb.img.Close()
	rec := e.poster("1")
	if !isPlaceholder(rec) {
		t.Fatalf("%d", rec.Code)
	}
	checkNoKey(t, e, rec)
}

func TestNoTMDBIDOrUnknownMovieOrModelDown(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	for _, id := range []string{"60", "999999999"} {
		if rec := e.poster(id); !isPlaceholder(rec) {
			t.Fatalf("%s: %d", id, rec.Code)
		}
	}
	e.Fake.SetDown(true)
	if rec := e.poster("4993"); !isPlaceholder(rec) {
		t.Fatalf("model down: %d", rec.Code)
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("TMDB asked about a movie without a TMDB id")
	}
}

// checkNoKey asserts the key is in neither the response nor the logs.
func checkNoKey(t *testing.T, e *env, rec *httptest.ResponseRecorder) {
	t.Helper()
	if strings.Contains(rec.Body.String(), testKey) || strings.Contains(fmt.Sprint(rec.Header()), testKey) {
		t.Fatal("key in response")
	}
	if strings.Contains(e.Logs.String(), testKey) {
		t.Fatalf("key in logs: %s", e.Logs.String())
	}
}

// Gate: the key never appears in logs or responses, including when every
// call fails with an error that carries the request URL.
func TestKeyNeverLeaks(t *testing.T) {
	e := newEnv(t, setup{key: testKey, timeout: 200 * time.Millisecond})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) {
		// Hang up mid-response so the client error includes the URL.
		hj, ok := w.(http.Hijacker)
		if !ok {
			return
		}
		conn, _, _ := hj.Hijack()
		_ = conn.Close()
	})
	for i := 1; i <= 3; i++ {
		checkNoKey(t, e, e.poster(strconv.Itoa(i)))
	}
	e.tmdb.api.Close()
	checkNoKey(t, e, e.poster("4"))
	if !strings.Contains(e.Logs.String(), "posters") {
		t.Fatal("failures were not logged at all (the check above proves nothing)")
	}
}

// ---------------------------------------------------------------- limits

// The global upstream limit: once spent, uncached posters get the
// placeholder without contacting TMDB; cached ones are still served.
func TestGlobalUpstreamLimit(t *testing.T) {
	e := newEnv(t, setup{key: testKey, limiter: ratelimit.NewGlobal(ratelimit.Config{Every: time.Hour, Burst: 2})})
	for _, id := range []string{"1", "2"} {
		if rec := e.poster(id); rec.Header().Get("Content-Type") != "image/jpeg" {
			t.Fatalf("%s: %q", id, rec.Header().Get("Content-Type"))
		}
	}
	api := e.tmdb.apiHits.Load()
	if rec := e.poster("3"); !isPlaceholder(rec) {
		t.Fatal("limit not applied")
	}
	if e.tmdb.apiHits.Load() != api {
		t.Fatal("TMDB contacted past the limit")
	}
	if rec := e.poster("1"); !isPoster(rec, 1) {
		t.Fatal("cached poster not served while limited")
	}
}

// posterEntries lists dir without the details cache's subdirectory.
func posterEntries(t *testing.T, dir string) []os.DirEntry {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	return slices.DeleteFunc(ents, func(e os.DirEntry) bool { return e.IsDir() && e.Name() == "details" })
}

func dirSize(t *testing.T, dir string) (int64, []string) {
	t.Helper()
	ents := posterEntries(t, dir)
	var total int64
	var names []string
	for _, e := range ents {
		info, err := e.Info()
		if err != nil {
			t.Fatal(err)
		}
		total += info.Size()
		names = append(names, e.Name())
	}
	return total, names
}

// Gate: eviction keeps the disk cache under its size cap, least recently
// used first.
func TestEvictionKeepsUnderCap(t *testing.T) {
	const capBytes = 3500 // room for three 1000-byte posters
	e := newEnv(t, setup{key: testKey, maxBytes: capBytes})
	for i := 1; i <= 3; i++ {
		e.poster(strconv.Itoa(i))
	}
	e.poster("1") // 1 is now most recently used; 2 is least
	for i := 4; i <= 20; i++ {
		if rec := e.poster(strconv.Itoa(i)); !isPoster(rec, byte(i)) {
			t.Fatalf("%d not served", i)
		}
		if total, names := dirSize(t, e.dir); total > capBytes {
			t.Fatalf("after %d: %d bytes on disk (%v)", i, total, names)
		}
		if i == 4 {
			_, names := dirSize(t, e.dir)
			if strings.Contains(strings.Join(names, ","), "2.jpg") || !strings.Contains(strings.Join(names, ","), "1.jpg") {
				t.Fatalf("evicted the wrong file: %v", names)
			}
		}
	}
	_, names := dirSize(t, e.dir)
	if len(names) != 3 {
		t.Fatalf("files %v", names)
	}
}

// The index is rebuilt from disk at startup, stray files are ignored and
// the cap is enforced on what was already there.
func TestCacheReloadsFromDisk(t *testing.T) {
	dir := t.TempDir()
	for i := 1; i <= 5; i++ {
		p := filepath.Join(dir, strconv.Itoa(i)+".jpg")
		if err := os.WriteFile(p, jpeg(1000, byte(i)), 0o644); err != nil {
			t.Fatal(err)
		}
		ts := time.Unix(int64(1_700_000_000+i), 0)
		_ = os.Chtimes(p, ts, ts)
	}
	_ = os.WriteFile(filepath.Join(dir, "notes.txt"), []byte("keep me"), 0o644)
	_ = os.WriteFile(filepath.Join(dir, ".tmp-123"), []byte("partial"), 0o644)
	c := newDiskCache(dir, 3500, nil)
	if c.total > 3500 {
		t.Fatalf("total %d", c.total)
	}
	for i, want := range []bool{false, false, true, true, true} {
		if _, ok := c.get(int64(i + 1)); ok != want {
			t.Errorf("poster %d cached = %v, want %v", i+1, ok, want)
		}
	}
	if _, err := os.Stat(filepath.Join(dir, "notes.txt")); err != nil {
		t.Error("unrelated file removed")
	}
	if _, err := os.Stat(filepath.Join(dir, ".tmp-123")); err == nil {
		t.Error("stale temporary file kept")
	}
}

// A poster larger than the whole cache is served but not stored.
func TestPosterLargerThanCacheNotStored(t *testing.T) {
	e := newEnv(t, setup{key: testKey, maxBytes: 500})
	if rec := e.poster("1"); !isPoster(rec, 1) {
		t.Fatal("not served")
	}
	if total, _ := dirSize(t, e.dir); total != 0 {
		t.Fatalf("%d bytes stored", total)
	}
}

func TestRegisterSignature(t *testing.T) {
	var _ func(*http.ServeMux, *app.Deps) = Register
	var _ modelclient.Client = nil
}
