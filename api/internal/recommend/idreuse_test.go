package recommend

import (
	"context"
	"encoding/json"
	"sync/atomic"
	"testing"

	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// Security audit SA1-1: a deleted account's cached recommendations (whose
// "because" lists name the movies it rated) must never reach a later account,
// even one that ends up with the same user ID and ratings_version.

// echoModel recommends movie 9999 "because" of every movie the caller rated.
type echoModel struct {
	modelclient.Client
	calls atomic.Int64
}

func (s *echoModel) Version(context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	return modelclient.VersionResponse{ArtifactVersion: "v"}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *echoModel) Recommend(_ context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	s.calls.Add(1)
	it := modelclient.Item{MovieID: 9999, Score: 1}
	for _, r := range req.Ratings {
		it.Because = append(it.Because, modelclient.Because{MovieID: r.MovieID, Weight: 1})
	}
	return modelclient.RecommendResponse{Model: req.Model, Items: []modelclient.Item{it}}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *echoModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	var ms []modelclient.MovieMeta
	for _, id := range ids {
		ms = append(ms, modelclient.MovieMeta{MovieID: id, Title: "t"})
	}
	return modelclient.BatchResponse{Movies: ms}, modelclient.Meta{ModelVersion: "v"}, nil
}

type idReuseEnv struct {
	*cachetest.Env
	model *echoModel
	inv   *invites.Store
}

func newIDReuseEnv(t *testing.T) *idReuseEnv {
	m := &echoModel{}
	e := cachetest.NewEnv(t, m, auth.Register, Register)
	return &idReuseEnv{Env: e, model: m, inv: invites.New(e.DB)}
}

// signup creates an account through the real signup route.
func (e *idReuseEnv) signup(t *testing.T, name string) (int64, string) {
	t.Helper()
	code, err := e.inv.Create(context.Background(), 1, 0, "")
	if err != nil {
		t.Fatal(err)
	}
	rec := e.Do("POST", "/api/auth/signup", map[string]string{"username": name, "password": "correct horse battery", "invite_code": code}, "")
	if rec.Code != 201 {
		t.Fatalf("signup %d %s", rec.Code, rec.Body.String())
	}
	var tok string
	for _, c := range rec.Result().Cookies() {
		if c.Name == "session" {
			tok = c.Value
		}
	}
	var id int64
	if err := e.DB.Reader.QueryRow("SELECT id FROM users WHERE username = ?", name).Scan(&id); err != nil {
		t.Fatal(err)
	}
	return id, tok
}

// victim rates five private movies (101..105), fetches recommendations (so
// they are cached) and deletes the account. It returns the victim's ID and
// created_at.
func (e *idReuseEnv) victim(t *testing.T) (id, createdAt int64) {
	t.Helper()
	id, tok := e.signup(t, "victim")
	if err := e.DB.Reader.QueryRow("SELECT created_at FROM users WHERE id = ?", id).Scan(&createdAt); err != nil {
		t.Fatal(err)
	}
	for i, mv := range []int64{101, 102, 103, 104, 105} {
		rate(t, e.DB, id, mv, 5, int64(1000+i))
	}
	if rec := get(e.Env, "/api/recommend?model=knn&k=5", tok); rec.Code != 200 {
		t.Fatalf("victim recommend %d %s", rec.Code, rec.Body.String())
	}
	if rec := e.Do("DELETE", "/api/me", nil, tok); rec.Code != 204 {
		t.Fatalf("delete %d", rec.Code)
	}
	return id, createdAt
}

// assertNoLeak rates five different movies as uid and checks that its
// recommendations were computed for it, not served from the victim's entry.
func (e *idReuseEnv) assertNoLeak(t *testing.T, uid int64, tok string) {
	t.Helper()
	for i, mv := range []int64{201, 202, 203, 204, 205} { // same count, so same ratings_version
		rate(t, e.DB, uid, mv, 3, int64(2000+i))
	}
	before := e.model.calls.Load()
	rec := get(e.Env, "/api/recommend?model=knn&k=5", tok)
	if rec.Code != 200 {
		t.Fatalf("recommend %d %s", rec.Code, rec.Body.String())
	}
	var body RecommendResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	for _, it := range body.Items {
		for _, b := range it.Because {
			if b.MovieID >= 101 && b.MovieID <= 105 {
				t.Errorf("LEAK: new account sees the deleted account's rated movie %d", b.MovieID)
			}
		}
	}
	if e.model.calls.Load() == before {
		t.Error("new account's recommendations were served from the cache")
	}
}

// Through the real signup path: the new account must not get the deleted
// account's ID, and must not see its cached recommendations.
func TestDeletedUserIDNotReusedAndCacheNotLeaked(t *testing.T) {
	e := newIDReuseEnv(t)
	e.signup(t, "alice")
	victimID, _ := e.victim(t)
	attackerID, attackerTok := e.signup(t, "attacker")
	if attackerID == victimID {
		t.Errorf("new account reused the deleted account's ID %d", victimID)
	}
	e.assertNoLeak(t, attackerID, attackerTok)
}

// Deleting an account drops its cached recommendations at once. Even if a
// row did come back with the same ID and created_at second (manual repair
// while the server runs), nothing cached reaches it: the retired ID is
// refused (429) before any lookup.
func TestAccountDeletionPurgesRecommendCache(t *testing.T) {
	m := &echoModel{}
	e, in := newRetireEnv(t, m)
	e.model = m
	victimID, createdAt := e.victim(t)
	if n := in.cache.entriesFor(victimID); n != 0 {
		t.Fatalf("%d cache entries left for the deleted account", n)
	}
	if _, err := e.DB.Writer.Exec("INSERT INTO users (id, username, pw_hash, created_at) VALUES (?, 'reborn', 'x', ?)", victimID, createdAt); err != nil {
		t.Fatal(err)
	}
	tok := newSession(t, e.Env, victimID)
	for i, mv := range []int64{201, 202, 203, 204, 205} {
		rate(t, e.DB, victimID, mv, 3, int64(2000+i))
	}
	rec := get(e.Env, "/api/recommend?model=knn&k=5", tok)
	if rec.Code != 429 {
		t.Fatalf("reborn ID: %d %s, want 429", rec.Code, rec.Body.String())
	}
}
