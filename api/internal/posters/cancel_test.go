package posters

import (
	"context"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

// Security audit SA5-4: a client that gives up mid-fetch must not leave the
// movie negatively cached (a cheap way to blank any poster for 10 minutes),
// and concurrent requests for one movie share one upstream fetch.

// blockImages makes the image host wait until release is closed, and
// reports each request on started.
func blockImages(e *env) (started chan struct{}, release chan struct{}) {
	started, release = make(chan struct{}, 16), make(chan struct{})
	img := e.tmdb.imgHandler
	e.tmdb.setImg(func(w http.ResponseWriter, r *http.Request) {
		started <- struct{}{}
		select {
		case <-release:
		case <-time.After(10 * time.Second):
		}
		img(w, r)
	})
	return started, release
}

func (e *env) posterCtx(ctx context.Context, id string) *httptest.ResponseRecorder {
	req := httptest.NewRequestWithContext(ctx, "GET", "/api/posters/"+id, nil)
	rec := httptest.NewRecorder()
	e.Handler.ServeHTTP(rec, req)
	return rec
}

func TestClientCancelDoesNotNegativelyCache(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	started, release := blockImages(e)

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		e.posterCtx(ctx, "7")
	}()
	<-started
	cancel() // the first client goes away mid-fetch
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("cancelled request did not return")
	}
	close(release)

	// A second client gets the real poster (from the fetch the first one
	// started, or a fresh one), not the placeholder.
	deadline := time.Now().Add(5 * time.Second)
	for {
		rec := e.poster("7")
		if isPoster(rec, 7) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("second client got the placeholder: %s %q", rec.Header().Get("Content-Type"), rec.Body.String())
		}
		time.Sleep(20 * time.Millisecond)
	}
	if n := e.tmdb.apiHits.Load(); n != 1 {
		t.Errorf("TMDB API called %d times, want 1 (the detached fetch is reused)", n)
	}
}

func TestConcurrentRequestsShareOneFetch(t *testing.T) {
	e := newEnv(t, setup{key: testKey})
	started, release := blockImages(e)

	const clients = 8
	var wg sync.WaitGroup
	results := make([]*httptest.ResponseRecorder, clients)
	for i := 0; i < clients; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			results[i] = e.poster("9")
		}(i)
	}
	<-started
	time.Sleep(100 * time.Millisecond) // let every client join
	close(release)
	wg.Wait()
	for i, rec := range results {
		if !isPoster(rec, 9) {
			t.Errorf("client %d got the placeholder", i)
		}
	}
	if n := e.tmdb.apiHits.Load(); n != 1 {
		t.Errorf("TMDB API called %d times for one movie, want 1", n)
	}
	if n := e.tmdb.imgHits.Load(); n != 1 {
		t.Errorf("TMDB image host called %d times for one movie, want 1", n)
	}
}
