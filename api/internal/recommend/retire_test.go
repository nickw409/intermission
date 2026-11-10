package recommend

import (
	"context"
	"net/http"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// PR #42 review M1 and test-validator note 14: after an account is deleted
// its cache entries and per-user bucket are gone and stay gone, even when a
// recommend request that started before the deletion finishes after it.

// heldModel blocks Recommend until release is closed.
type heldModel struct {
	echoModel
	entered chan struct{}
	release chan struct{}
}

func (m *heldModel) Recommend(ctx context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	m.entered <- struct{}{}
	<-m.release
	return m.echoModel.Recommend(ctx, req)
}

type internals struct {
	cache   *lru
	buckets *ratelimit.Keyed
}

func newRetireEnv(t *testing.T, model modelclient.Client) (*idReuseEnv, *internals) {
	t.Helper()
	in := &internals{}
	reg := func(mux *http.ServeMux, d *app.Deps) {
		register(mux, d, options{observe: func(c *lru, b *ratelimit.Keyed) { in.cache, in.buckets = c, b }})
	}
	e := cachetest.NewEnv(t, model, auth.Register, reg)
	env := &idReuseEnv{Env: e, inv: invites.New(e.DB)}
	return env, in
}

func (c *lru) entriesFor(uid int64) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.users[uid]
}

// Deleting an account drops its per-user bucket, and a request that slips
// through afterwards does not bring the bucket back.
func TestAccountDeletionRetiresUserBucket(t *testing.T) {
	e, in := newRetireEnv(t, &echoModel{})
	id, tok := e.signup(t, "bucketuser")
	for i, mv := range []int64{1, 2, 3, 4, 5} {
		rate(t, e.DB, id, mv, 4, int64(i))
	}
	if rec := get(e.Env, "/api/recommend?model=knn&k=5", tok); rec.Code != 200 {
		t.Fatalf("recommend %d", rec.Code)
	}
	if in.buckets.Len() != 1 {
		t.Fatalf("user buckets = %d after one request, want 1", in.buckets.Len())
	}
	if rec := e.Do("DELETE", "/api/me", nil, tok); rec.Code != 204 {
		t.Fatalf("delete %d", rec.Code)
	}
	if in.buckets.Len() != 0 {
		t.Fatalf("user buckets = %d after the account was deleted, want 0", in.buckets.Len())
	}
	if ok, _ := in.buckets.Allow(userBucket(id)); ok {
		t.Error("a deleted user's bucket key was allowed")
	}
	if in.buckets.Len() != 0 {
		t.Error("a deleted user's bucket was recreated")
	}
}

// The model call of an in-flight request is held while the account is
// deleted; when it completes, nothing for that user enters the cache.
func TestInFlightRecommendCannotRepopulateAfterDeletion(t *testing.T) {
	m := &heldModel{entered: make(chan struct{}, 1), release: make(chan struct{})}
	e, in := newRetireEnv(t, m)
	id, tok := e.signup(t, "racer")
	for i, mv := range []int64{1, 2, 3, 4, 5} {
		rate(t, e.DB, id, mv, 4, int64(i))
	}
	done := make(chan int, 1)
	go func() { done <- get(e.Env, "/api/recommend?model=knn&k=5", tok).Code }()
	select {
	case <-m.entered:
	case <-time.After(5 * time.Second):
		t.Fatal("model was never called")
	}
	if rec := e.Do("DELETE", "/api/me", nil, tok); rec.Code != 204 {
		t.Fatalf("delete %d", rec.Code)
	}
	close(m.release)
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("recommend did not finish")
	}
	if n := in.cache.entriesFor(id); n != 0 {
		t.Fatalf("%d cache entries for the deleted user after the in-flight request finished", n)
	}
}
