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
		{"img 429", false, true, 429},
		{"api 502", true, false, 502},
		{"api 504", true, false, 504},
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
			final := e.poster("7")
			if !isPoster(final, 7) {
				t.Fatal("still the placeholder after busyTTL")
			}
			checkNoKey(t, e, final)
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
			rec = e.details("7")
			decodeDetails(t, rec)
			if e.tmdb.apiHits.Load() != hits {
				t.Fatal("asked TMDB again within busyTTL")
			}
			if got := rec.Header().Get("Cache-Control"); got != "public, max-age=1" {
				t.Errorf("remembered empty details Cache-Control %q, want what is left of busyTTL", got)
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

func TestRememberedCacheControl(t *testing.T) {
	for left, want := range map[time.Duration]string{
		999 * time.Millisecond:    "no-store", // never past the entry
		time.Second:               "public, max-age=1",
		1500 * time.Millisecond:   "public, max-age=1", // rounded down
		busyTTL:                   "public, max-age=30",
		time.Minute:               "public, max-age=60",
		negativeTTL - time.Minute: "public, max-age=300", // capped
		negativeTTL:               "public, max-age=300",
	} {
		if got := rememberedCacheControl(left); got != want {
			t.Errorf("%v left: %q, want %q", left, got, want)
		}
	}
}

func TestPosterRememberedNeverOutlastsEntry(t *testing.T) {
	clk := &testClock{t: time.Unix(1_800_000_000, 0)}
	e := newEnv(t, setup{key: testKey, now: clk.now})
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "busy", 503) })
	e.poster("7")
	clk.advance(busyTTL - 1500*time.Millisecond)
	if got := e.poster("7").Header().Get("Cache-Control"); got != "public, max-age=1" {
		t.Errorf("1.5 s left: %q", got)
	}
	clk.advance(time.Second)
	if got := e.poster("7").Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("0.5 s left: %q", got)
	}

	// A lasting failure remembered for 9 more minutes is capped at 300 s.
	e.tmdb.setAPI(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"poster_path":null}`) })
	e.poster("5")
	clk.advance(time.Minute)
	if got := e.poster("5").Header().Get("Cache-Control"); got != "public, max-age=300" {
		t.Errorf("9 min left: %q", got)
	}
}

// Lasting TMDB failures keep the 10-minute memory and the 300 s placeholder.
func TestPosterLastingFailures(t *testing.T) {
	for name, c := range map[string]struct{ api, img http.HandlerFunc }{
		"api 404":      {api: func(w http.ResponseWriter, r *http.Request) { http.NotFound(w, r) }},
		"api 501":      {api: func(w http.ResponseWriter, r *http.Request) { http.Error(w, "x", 501) }},
		"api not json": {api: func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "<html>") }},
		"img not jpeg": {img: func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "<svg/>") }},
	} {
		t.Run(name, func(t *testing.T) {
			clk := &testClock{t: time.Unix(1_800_000_000, 0)}
			e := newEnv(t, setup{key: testKey, now: clk.now})
			if c.api != nil {
				e.tmdb.setAPI(c.api)
			}
			if c.img != nil {
				e.tmdb.setImg(c.img)
			}
			rec := e.poster("7")
			if !isPlaceholder(rec) || rec.Header().Get("Cache-Control") != "public, max-age=300" {
				t.Fatalf("%s %q", rec.Header().Get("Content-Type"), rec.Header().Get("Cache-Control"))
			}
			hits := e.tmdb.apiHits.Load()
			clk.advance(busyTTL + time.Second)
			e.poster("7")
			if e.tmdb.apiHits.Load() != hits {
				t.Fatal("lasting failure remembered only for busyTTL")
			}
		})
	}
}

// recs-model down: the lookup failed, nothing is remembered, and browsers
// keep the answer only briefly.
func TestLookupFailureShortCache(t *testing.T) {
	e := detailsEnv(t, setup{key: testKey}, fellowship)
	e.Fake.SetDown(true)
	if rec := e.poster("999999"); !isPlaceholder(rec) || rec.Header().Get("Cache-Control") != "public, max-age=30" {
		t.Errorf("poster: %q", rec.Header().Get("Cache-Control"))
	}
	if rec := e.details("999998"); rec.Header().Get("Cache-Control") != "public, max-age=30" {
		t.Errorf("details: %q", rec.Header().Get("Cache-Control"))
	}
}

// The lookup budget running out is ours too: no-store.
func TestLookupLimitedNoStore(t *testing.T) {
	lookup := ratelimit.NewGlobal(ratelimit.Config{Every: time.Hour, Burst: 1})
	e := detailsEnv(t, setup{key: testKey, lookup: lookup}, fellowship)
	if rec := e.poster("7"); !isPoster(rec, 7) {
		t.Fatal("first poster")
	}
	if rec := e.poster("8"); !isPlaceholder(rec) || rec.Header().Get("Cache-Control") != "no-store" {
		t.Errorf("poster: %q", rec.Header().Get("Cache-Control"))
	}
	if rec := e.details("8"); rec.Header().Get("Cache-Control") != "no-store" {
		t.Errorf("details: %q", rec.Header().Get("Cache-Control"))
	}
}

func TestNegCacheRemaining(t *testing.T) {
	clk := &testClock{t: time.Unix(1_800_000_000, 0)}
	n := newNegCache(10, negativeTTL, clk.now)
	n.addFor(1, busyTTL)
	n.add(2)
	if left, ok := n.remaining(1); !ok || left != busyTTL {
		t.Fatalf("1: %v %v", left, ok)
	}
	clk.advance(busyTTL)
	if _, ok := n.remaining(1); ok || n.has(1) {
		t.Fatal("entry alive at its expiry")
	}
	if left, ok := n.remaining(2); !ok || left != negativeTTL-busyTTL || !n.has(2) {
		t.Fatalf("2: %v %v", left, ok)
	}
}

// Audit M1 bound: a live negativeTTL entry can't be evicted early, because
// every entry costs a lookup token and the lookup budget can't add
// negativeEntries entries within negativeTTL.
func TestNegativeCacheOutlastsLookupBudget(t *testing.T) {
	maxAdds := int64(negativeTTL/LookupLimit.Every) + int64(LookupLimit.Burst)
	if maxAdds >= negativeEntries {
		t.Fatalf("LookupLimit allows %d entries in negativeTTL, negativeEntries is %d", maxAdds, negativeEntries)
	}
}
