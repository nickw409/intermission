package posters

import (
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// TMDB answering 429 or 5xx is temporary: the movie is remembered for
// busyTTL (so a client looping on it can't hammer TMDB, audit M1), not for
// negativeTTL, and browsers are told to keep the placeholder no longer.

func TestPosterTMDBBusyRememberedBriefly(t *testing.T) {
	for _, c := range []struct {
		name     string
		api, img bool
		status   int
	}{
		{"api 429", true, false, 429},
		{"api 500", true, false, 500},
		{"api 503", true, false, 503},
		{"img 503", false, true, 503},
	} {
		t.Run(c.name, func(t *testing.T) {
			clk := &testClock{t: time.Unix(1_800_000_000, 0)}
			e := newEnv(t, setup{key: testKey, now: clk.now})
			api, img := e.tmdb.apiHandler, e.tmdb.imgHandler
			busy := func(w http.ResponseWriter, r *http.Request) { http.Error(w, "busy", c.status) }
			if c.api {
				e.tmdb.setAPI(busy)
			}
			if c.img {
				e.tmdb.setImg(busy)
			}
			rec := e.poster("7")
			if !isPlaceholder(rec) {
				t.Fatalf("%d %s", rec.Code, rec.Header().Get("Content-Type"))
			}
			if got := rec.Header().Get("Cache-Control"); got != "public, max-age=30" {
				t.Errorf("Cache-Control %q, want public, max-age=30", got)
			}
			// TMDB recovers at once, but within busyTTL the movie is not
			// asked about again.
			e.tmdb.setAPI(api)
			e.tmdb.setImg(img)
			hits := e.tmdb.apiHits.Load()
			clk.advance(busyTTL - time.Second)
			rec = e.poster("7")
			if !isPlaceholder(rec) || e.tmdb.apiHits.Load() != hits {
				t.Fatal("asked TMDB again within busyTTL")
			}
			if got := rec.Header().Get("Cache-Control"); got != "public, max-age=1" {
				t.Errorf("remembered placeholder Cache-Control %q, want what is left of busyTTL", got)
			}
			// Then the poster shows up: not hidden for the 10-minute TTL.
			clk.advance(2 * time.Second)
			if rec := e.poster("7"); !isPoster(rec, 7) {
				t.Fatal("still the placeholder after busyTTL")
			}
			checkNoKey(t, e, rec)
		})
	}
}

func TestPosterNoPosterStillRememberedLong(t *testing.T) {
	clk := &testClock{t: time.Unix(1_800_000_000, 0)}
	e := newEnv(t, setup{key: testKey, now: clk.now})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"poster_path":null}`) })
	if rec := e.poster("5"); rec.Header().Get("Cache-Control") != "public, max-age=300" {
		t.Errorf("Cache-Control %q", rec.Header().Get("Cache-Control"))
	}
	clk.advance(negativeTTL - time.Minute)
	rec := e.poster("5")
	if e.tmdb.apiHits.Load() != 1 {
		t.Fatal("posterless movie asked about again within negativeTTL")
	}
	// The browser may keep it only for what is left of negativeTTL.
	if got := rec.Header().Get("Cache-Control"); got != "public, max-age=60" {
		t.Errorf("Cache-Control %q, want public, max-age=60", got)
	}
}

func TestPosterTimeoutShortCache(t *testing.T) {
	e := newEnv(t, setup{key: testKey, timeout: 50 * time.Millisecond})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { <-r.Context().Done() })
	rec := e.poster("7")
	if !isPlaceholder(rec) {
		t.Fatal(rec.Code)
	}
	if got := rec.Header().Get("Cache-Control"); got != "public, max-age=30" {
		t.Errorf("timeout placeholder Cache-Control %q", got)
	}
}

func TestPosterLimitedNoStore(t *testing.T) {
	lim := ratelimit.NewGlobal(ratelimit.Config{Every: 100 * time.Millisecond, Burst: 1})
	e := newEnv(t, setup{key: testKey, limiter: lim})
	if rec := e.poster("7"); !isPoster(rec, 7) {
		t.Fatal("first poster")
	}
	rec := e.poster("8")
	if !isPlaceholder(rec) {
		t.Fatal("over budget but fetched")
	}
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("rate-limited placeholder Cache-Control %q, want no-store", got)
	}
	time.Sleep(150 * time.Millisecond)
	if rec := e.poster("8"); !isPoster(rec, 8) {
		t.Fatal("limited placeholder was remembered")
	}
}

func TestDetailsTMDBBusyRememberedBriefly(t *testing.T) {
	for _, status := range []int{429, 500, 503} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			clk := &testClock{t: time.Unix(1_800_000_000, 0)}
			e := detailsEnv(t, setup{key: testKey, now: clk.now}, fellowship)
			e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "busy", status) })
			rec := e.details("7")
			if d := decodeDetails(t, rec); !isEmptyDetails(d, 7) {
				t.Fatalf("%+v", d)
			}
			if got := rec.Header().Get("Cache-Control"); got != "public, max-age=30" {
				t.Errorf("Cache-Control %q", got)
			}
			e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, fellowship("1007")) })
			hits := e.tmdb.apiHits.Load()
			clk.advance(busyTTL - time.Second)
			decodeDetails(t, e.details("7"))
			if e.tmdb.apiHits.Load() != hits {
				t.Fatal("asked TMDB again within busyTTL")
			}
			clk.advance(2 * time.Second)
			if d := decodeDetails(t, e.details("7")); deref(d.Overview) != "Movie 1007." {
				t.Fatalf("after TMDB %d the movie stayed empty past busyTTL", status)
			}
			if e.Logs.String() == "" {
				t.Error("busy TMDB not logged")
			}
		})
	}
}
