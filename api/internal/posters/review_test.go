package posters

import (
	"net/http"
	"strings"
	"testing"
	"time"
)

// PR #42 review L2 and SA3-10.

// A TMDB timeout is a passing condition, not "this movie has no poster":
// the next request tries again instead of getting the placeholder for ten
// minutes.
func TestTMDBTimeoutNotNegativelyCached(t *testing.T) {
	e := newEnv(t, setup{key: testKey, timeout: 200 * time.Millisecond})
	img := e.tmdb.imgHandler
	slow := make(chan struct{})
	e.tmdb.setImg(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-slow:
		case <-time.After(2 * time.Second):
		}
		img(w, r)
	})
	if rec := e.poster("5"); !isPlaceholder(rec) {
		t.Fatalf("slow TMDB: want the placeholder, got %s", rec.Header().Get("Content-Type"))
	}
	close(slow) // TMDB is fast again
	e.tmdb.setImg(img)
	if rec := e.poster("5"); !isPoster(rec, 5) {
		t.Fatal("after a TMDB timeout the movie stayed on the placeholder (negatively cached)")
	}
}

// A panic inside a shared fetch becomes that call's error; waiters are
// released and the process keeps running.
func TestFlightPanicBecomesError(t *testing.T) {
	var g flightGroup
	c := g.do(1, func() ([]byte, error) { panic("boom") })
	select {
	case <-c.done:
	case <-time.After(2 * time.Second):
		t.Fatal("waiters never released after a panic")
	}
	if c.err == nil || !strings.Contains(c.err.Error(), "panic") {
		t.Fatalf("err = %v, want a panic error", c.err)
	}
	// The group still works for the same ID.
	c = g.do(1, func() ([]byte, error) { return []byte("ok"), nil })
	<-c.done
	if c.err != nil || string(c.img) != "ok" {
		t.Fatalf("after a panic: %q %v", c.img, c.err)
	}
}

func TestPlaceholderCSPForbidsFraming(t *testing.T) {
	e := newEnv(t, setup{key: ""})
	rec := e.poster("5")
	csp := rec.Header().Get("Content-Security-Policy")
	for _, want := range []string{"default-src 'none'", "sandbox", "frame-ancestors 'none'"} {
		if !strings.Contains(csp, want) {
			t.Errorf("placeholder CSP %q lacks %q", csp, want)
		}
	}
}
