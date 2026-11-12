package posters

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// TMDB's /3/movie/{id}?append_to_response=credits for the getMovieDetails
// "ok" fixture: cast out of billing order, an empty character, a
// non-director crew member and a cast list longer than ten are all there
// on purpose.
const fellowshipTMDB = `{
  "id": 120, "title": "The Lord of the Rings: The Fellowship of the Ring",
  "overview": "A quiet hobbit inherits a dangerous ring and sets out with eight companions to destroy it before its maker can reclaim it.",
  "tagline": "One ring to rule them all.",
  "runtime": 179, "release_date": "2001-12-18", "poster_path": "/p120.jpg",
  "credits": {
    "cast": [
      {"name": "Ian McKellen", "character": "Gandalf", "order": 1},
      {"name": "Elijah Wood", "character": "Frodo Baggins", "order": 0},
      {"name": "Viggo Mortensen", "character": "Aragorn", "order": 2},
      {"name": "Sean Astin", "character": "Samwise Gamgee", "order": 3},
      {"name": "Cate Blanchett", "character": "", "order": 4}
    ],
    "crew": [
      {"name": "Howard Shore", "job": "Original Music Composer"},
      {"name": "Peter Jackson", "job": "Director"},
      {"name": "Peter Jackson", "job": "Producer"}
    ]
  }
}`

// detailsEnv is a poster env that also knows movie 4993 (TMDB 120) and
// answers TMDB detail requests with body (a function of the TMDB id).
func detailsEnv(t *testing.T, s setup, body func(tmdbID string) string) *env {
	t.Helper()
	e := newEnv(t, s)
	m := cachetest.Movie(4993, "Lord of the Rings: The Fellowship of the Ring, The", 2001)
	tm := int64(120)
	m.TMDBID = &tm
	cachetest.SeedMovies(t, e.DB, m)
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, body(strings.TrimPrefix(r.URL.Path, "/3/movie/")))
	})
	return e
}

func fellowship(id string) string {
	if id == "120" {
		return fellowshipTMDB
	}
	return fmt.Sprintf(`{"id":%s,"overview":"Movie %s.","poster_path":"/p%s.jpg","credits":{"cast":[],"crew":[]}}`, id, id, id)
}

func (e *env) details(id string) *httptest.ResponseRecorder {
	return e.Do("GET", "/api/movies/"+id+"/details", nil, "")
}

func decodeDetails(t *testing.T, rec *httptest.ResponseRecorder) MovieDetails {
	t.Helper()
	if rec.Code != 200 {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Fatalf("content type %q", ct)
	}
	dec := json.NewDecoder(bytes.NewReader(rec.Body.Bytes()))
	dec.DisallowUnknownFields()
	var d MovieDetails
	if err := dec.Decode(&d); err != nil {
		t.Fatalf("body %s: %v", rec.Body.String(), err)
	}
	if d.Directors == nil || d.Cast == nil {
		t.Fatalf("directors and cast must be arrays, not null: %s", rec.Body.String())
	}
	return d
}

func isEmptyDetails(d MovieDetails, id int64) bool {
	return d.MovieID == id && d.Overview == nil && d.Tagline == nil && d.RuntimeMinutes == nil &&
		d.ReleaseDate == nil && len(d.Directors) == 0 && len(d.Cast) == 0
}

func sameJSON(t *testing.T, got, want []byte) {
	t.Helper()
	var g, w any
	if err := json.Unmarshal(got, &g); err != nil {
		t.Fatalf("body %q", got)
	}
	_ = json.Unmarshal(want, &w)
	gb, _ := json.Marshal(g)
	wb, _ := json.Marshal(w)
	if !bytes.Equal(gb, wb) {
		t.Errorf("body %s\nwant %s", gb, wb)
	}
}

// ---------------------------------------------------------------- contract

func TestDetailsContractFixtures(t *testing.T) {
	for _, c := range cachetest.PublicCases(t, "getMovieDetails") {
		t.Run(c.Name, func(t *testing.T) {
			key := ""
			if c.Name == "ok" {
				key = testKey
			}
			e := detailsEnv(t, setup{key: key}, fellowship)
			rec := e.Replay(c, "")
			if rec.Code != c.Response.Status {
				t.Fatalf("status %d, want %d: %s", rec.Code, c.Response.Status, rec.Body.String())
			}
			sameJSON(t, rec.Body.Bytes(), c.Response.Body)
		})
	}
}

func TestDetailsBadMovieIDs(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	for _, id := range []string{"abc", "0", "-1", "1.5", "2147483648", "1e3", "%2e%2e"} {
		if rec := e.details(id); rec.Code != 400 && rec.Code != 404 {
			t.Errorf("%s: %d", id, rec.Code)
		}
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("bad IDs reached TMDB")
	}
}

// ---------------------------------------------------------------- success

func TestDetailsFetchAndCache(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	rec := e.details("4993")
	d := decodeDetails(t, rec)
	if d.Overview == nil || !strings.HasPrefix(*d.Overview, "A quiet hobbit") {
		t.Fatalf("overview %v", d.Overview)
	}
	if got := rec.Header().Get("Cache-Control"); got != "public, max-age=86400" {
		t.Errorf("Cache-Control %q", got)
	}
	if !e.tmdb.sawKey.Load() {
		t.Fatal("key not sent to TMDB")
	}
	e.tmdb.mu.Lock()
	q := e.tmdb.lastAPIQuery
	e.tmdb.mu.Unlock()
	if !strings.Contains(q, "append_to_response=credits") {
		t.Errorf("TMDB query %q does not ask for credits (one request for details and cast)", q)
	}
	if e.tmdb.imgHits.Load() != 0 {
		t.Error("details fetched an image")
	}
	if _, err := os.Stat(filepath.Join(e.dir, "details", "4993.json")); err != nil {
		t.Fatalf("not cached on disk: %v", err)
	}
	hits := e.tmdb.apiHits.Load()
	again := e.details("4993")
	if !bytes.Equal(again.Body.Bytes(), rec.Body.Bytes()) {
		t.Fatalf("cached body differs:\n%s\n%s", again.Body.String(), rec.Body.String())
	}
	if e.tmdb.apiHits.Load() != hits {
		t.Fatal("second request went to TMDB")
	}
	// The poster cache is a sibling and unaffected.
	if rec := e.poster("4993"); rec.Header().Get("Content-Type") != "image/jpeg" {
		t.Fatalf("poster after details: %d %s", rec.Code, rec.Header().Get("Content-Type"))
	}
}

func TestDetailsCacheSurvivesRestart(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	want := decodeDetails(t, e.details("7"))
	// A second handler over the same directory (a restart) serves from disk.
	e2 := newEnv(t, setup{key: testKey, dir: e.dir})
	got := decodeDetails(t, e2.details("7"))
	if e2.tmdb.apiHits.Load() != 0 {
		t.Fatal("restart refetched cached details")
	}
	if *got.Overview != *want.Overview {
		t.Fatalf("%q vs %q", *got.Overview, *want.Overview)
	}
}

func TestDetailsCorruptCacheFileRefetched(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	decodeDetails(t, e.details("7"))
	path := filepath.Join(e.dir, "details", "7.json")
	for _, junk := range []string{"not json", `{"movieId":8,"overview":null,"tagline":null,"runtime_minutes":null,"release_date":null,"directors":[],"cast":[]}`} {
		if err := os.WriteFile(path, []byte(junk), 0o600); err != nil {
			t.Fatal(err)
		}
		hits := e.tmdb.apiHits.Load()
		d := decodeDetails(t, e.details("7"))
		if d.MovieID != 7 || d.Overview == nil || *d.Overview != "Movie 1007." {
			t.Fatalf("%q: got %+v", junk, d)
		}
		if e.tmdb.apiHits.Load() != hits+1 {
			t.Fatalf("%q: corrupt cache file was served instead of refetched", junk)
		}
	}
}

// ---------------------------------------------------------------- untrusted TMDB text

func TestDetailsSanitized(t *testing.T) {
	long := strings.Repeat("é", 2500)
	var cast, crew []string
	for i := 14; i >= 0; i-- { // reverse billing order
		cast = append(cast, fmt.Sprintf(`{"name":"Actor %d","character":"Role %d","order":%d}`, i, i, i))
	}
	cast = append(cast, `{"name":"   ","character":"Nobody","order":-1}`) // blank name: skipped
	for i := range 5 {
		crew = append(crew, fmt.Sprintf(`{"name":"Director %d","job":"Director"}`, i))
	}
	raw := fmt.Sprintf(`{
	  "overview": %q,
	  "tagline": "  \u0007Bell\u202eand\u0000 tab\there\r\n ",
	  "runtime": 0,
	  "release_date": "2001-13-45",
	  "credits": {"cast": [%s], "crew": [%s]}
	}`, long, strings.Join(cast, ","), strings.Join(crew, ","))
	e := detailsEnv(t, setup{key: testKey}, func(string) string { return raw })
	d := decodeDetails(t, e.details("7"))

	if d.Overview == nil || utf8.RuneCountInString(*d.Overview) != maxOverviewRunes || !strings.HasSuffix(*d.Overview, "…") {
		t.Errorf("overview not truncated to %d runes with an ellipsis", maxOverviewRunes)
	}
	if d.Tagline == nil || *d.Tagline != "Belland tab here" {
		t.Errorf("tagline %q: control and bidi characters must go, tabs become spaces, ends trimmed", deref(d.Tagline))
	}
	if d.RuntimeMinutes != nil {
		t.Errorf("runtime 0 should be null, got %d", *d.RuntimeMinutes)
	}
	if d.ReleaseDate != nil {
		t.Errorf("impossible date passed through: %q", *d.ReleaseDate)
	}
	if len(d.Cast) != maxCast {
		t.Fatalf("cast %d, want %d", len(d.Cast), maxCast)
	}
	for i, c := range d.Cast {
		if c.Name != fmt.Sprintf("Actor %d", i) {
			t.Errorf("cast[%d] = %q: not in billing order", i, c.Name)
		}
	}
	if len(d.Directors) != maxDirectors {
		t.Errorf("directors %v, want %d", d.Directors, maxDirectors)
	}
}

func TestDetailsEmptyStringsAndNullsBecomeNull(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, func(string) string {
		return `{"overview":"","tagline":null,"runtime":null,"release_date":"","credits":{"cast":[{"name":"A","character":null,"order":0}],"crew":[]}}`
	})
	d := decodeDetails(t, e.details("7"))
	if d.Overview != nil || d.Tagline != nil || d.RuntimeMinutes != nil || d.ReleaseDate != nil {
		t.Fatalf("%+v", d)
	}
	if len(d.Cast) != 1 || d.Cast[0].Character != nil {
		t.Fatalf("cast %+v", d.Cast)
	}
}

func TestDetailsHTMLIsPassedAsText(t *testing.T) {
	// recs-api returns text; the browser renders it as text. What matters
	// here is that JSON encoding keeps it inert if a response is ever
	// opened directly (json.Marshal escapes <, > and &).
	e := detailsEnv(t, setup{key: testKey}, func(string) string {
		return `{"overview":"<script>alert(1)</script>","credits":{"cast":[],"crew":[]}}`
	})
	rec := e.details("7")
	if strings.Contains(rec.Body.String(), "<script>") {
		t.Fatalf("raw markup in body: %s", rec.Body.String())
	}
	if d := decodeDetails(t, rec); d.Overview == nil || *d.Overview != "<script>alert(1)</script>" {
		t.Fatalf("overview %v", d.Overview)
	}
}

// ---------------------------------------------------------------- failures: always 200, empty

func TestDetailsWithoutKeyNeverCallsTMDB(t *testing.T) {
	e := detailsEnv(t, setup{}, fellowship)
	rec := e.details("4993")
	if !isEmptyDetails(decodeDetails(t, rec), 4993) {
		t.Fatalf("%s", rec.Body.String())
	}
	if got := rec.Header().Get("Cache-Control"); got != "public, max-age=300" {
		t.Errorf("empty answer Cache-Control %q", got)
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("TMDB contacted without a key")
	}
}

func TestDetailsNoTMDBIDOrUnknownMovie(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	for _, id := range []int64{60, 999999} { // 60 has no TMDB id; 999999 is unknown
		for range 2 {
			if d := decodeDetails(t, e.details(fmt.Sprint(id))); !isEmptyDetails(d, id) {
				t.Fatalf("%d: %+v", id, d)
			}
		}
	}
	if e.tmdb.apiHits.Load() != 0 {
		t.Fatal("TMDB contacted for a movie without a TMDB id")
	}
}

func TestDetailsUpstreamFailures(t *testing.T) {
	cases := map[string]http.HandlerFunc{
		"500":      func(w http.ResponseWriter, r *http.Request) { http.Error(w, "x", 500) },
		"404":      func(w http.ResponseWriter, r *http.Request) { http.NotFound(w, r) },
		"not json": func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "<html>") },
		"too big": func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprintf(w, `{"overview":%q}`, strings.Repeat("a", maxDetailsBytes+10))
		},
	}
	for name, h := range cases {
		t.Run(name, func(t *testing.T) {
			e := detailsEnv(t, setup{key: testKey}, fellowship)
			e.tmdb.setAPI(h)
			rec := e.details("7")
			if !isEmptyDetails(decodeDetails(t, rec), 7) {
				t.Fatalf("%s", rec.Body.String())
			}
			if got := rec.Header().Get("Cache-Control"); got != "public, max-age=300" {
				t.Errorf("Cache-Control %q", got)
			}
			// Remembered for a while: no second TMDB call, nothing on disk.
			hits := e.tmdb.apiHits.Load()
			decodeDetails(t, e.details("7"))
			if e.tmdb.apiHits.Load() != hits {
				t.Error("failure not remembered; TMDB called again at once")
			}
			if _, err := os.Stat(filepath.Join(e.dir, "details", "7.json")); err == nil {
				t.Error("a failure was cached on disk")
			}
			if strings.Contains(e.Logs.String(), testKey) {
				t.Fatal("TMDB key in logs")
			}
		})
	}
}

func TestDetailsNegativeCacheExpires(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	clock := func() time.Time { return now }
	e := detailsEnv(t, setup{key: testKey, now: clock}, fellowship)
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "x", 500) })
	decodeDetails(t, e.details("7"))
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, fellowship("1007")) })
	now = now.Add(negativeTTL + time.Second)
	if d := decodeDetails(t, e.details("7")); d.Overview == nil {
		t.Fatal("still empty after the negative TTL")
	}
}

func TestDetailsShareTheUpstreamBudget(t *testing.T) {
	// One token: the poster takes it, so details for another movie get
	// the empty answer (not cached, not remembered as "no details").
	lim := ratelimit.NewGlobal(ratelimit.Config{Every: time.Hour, Burst: 1})
	e := detailsEnv(t, setup{key: testKey, limiter: lim}, fellowship)
	if rec := e.poster("7"); !isPoster(rec, 7) {
		t.Fatal("poster")
	}
	if d := decodeDetails(t, e.details("8")); !isEmptyDetails(d, 8) {
		t.Fatalf("over budget but fetched: %+v", d)
	}
	if e.tmdb.apiHits.Load() != 1 {
		t.Fatalf("TMDB API hits %d, want 1", e.tmdb.apiHits.Load())
	}
}

func TestDetailsTimeoutNotNegativelyCached(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey, timeout: 50 * time.Millisecond}, fellowship)
	slow := make(chan struct{})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-slow:
		case <-r.Context().Done():
		}
	})
	if d := decodeDetails(t, e.details("7")); !isEmptyDetails(d, 7) {
		t.Fatal("slow TMDB gave details")
	}
	close(slow)
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, fellowship("1007")) })
	if d := decodeDetails(t, e.details("7")); d.Overview == nil {
		t.Fatal("after a timeout the movie stayed empty (negatively cached)")
	}
}

func deref(s *string) string {
	if s == nil {
		return "<nil>"
	}
	return *s
}
