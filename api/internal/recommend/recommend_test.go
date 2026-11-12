package recommend

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

func newEnv(t *testing.T, model modelclient.Client) *cachetest.Env {
	t.Helper()
	return cachetest.NewEnv(t, model, Register)
}

func rate(t *testing.T, d *db.DB, uid, movie int64, rating float64, updated int64) {
	t.Helper()
	if _, err := d.Writer.Exec("INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)", uid, movie, rating, updated); err != nil {
		t.Fatal(err)
	}
	if _, err := d.Writer.Exec("UPDATE users SET ratings_version = ratings_version + 1 WHERE id = ?", uid); err != nil {
		t.Fatal(err)
	}
}

// seedAlice gives uid the five ratings in contracts/fixtures/model/recommend.json
// (newest first: 260, 2571, 296, 1, 318).
func seedAlice(t *testing.T, d *db.DB, uid int64) {
	t.Helper()
	for _, r := range []struct {
		m  int64
		r  float64
		ts int64
	}{{260, 5, 1794256872}, {2571, 4.5, 1794256855}, {296, 4.5, 1794256831}, {1, 4, 1794256802}, {318, 4, 1794256787}} {
		rate(t, d, uid, r.m, r.r, r.ts)
	}
}

// seedBob gives uid the two ratings of the ok_cold_start_pop model fixture
// (newest first: 1, 260).
func seedBob(t *testing.T, d *db.DB, uid int64) {
	t.Helper()
	rate(t, d, uid, 260, 5, 1794256700)
	rate(t, d, uid, 1, 4, 1794256800)
}

func get(e *cachetest.Env, path, token string) *httptest.ResponseRecorder {
	return e.Do("GET", path, nil, token)
}

// ---------------------------------------------------------------- contract

// replay sends a public fixture case and checks status, exact JSON body and
// headers (Retry-After by shape: a whole number of seconds, 1..60).
func replay(t *testing.T, e *cachetest.Env, c cachetest.Case, token string) *httptest.ResponseRecorder {
	t.Helper()
	target := c.Request.Path
	if len(c.Request.Query) > 0 {
		q := url.Values{}
		for k, v := range c.Request.Query {
			q.Set(k, v)
		}
		target += "?" + q.Encode()
	}
	req := httptest.NewRequest(c.Request.Method, target, nil)
	req.Host = cachetest.Host
	for k, v := range c.Request.Headers {
		req.Header.Set(k, strings.ReplaceAll(v, cachetest.FixtureToken, token))
	}
	rec := httptest.NewRecorder()
	e.Handler.ServeHTTP(rec, req)
	if rec.Code != c.Response.Status {
		t.Fatalf("status %d, want %d (%s)", rec.Code, c.Response.Status, rec.Body.String())
	}
	if rec.Header().Get("X-Request-ID") == "" {
		t.Error("no X-Request-ID")
	}
	for k, v := range c.Response.Headers {
		got := rec.Header().Get(k)
		if k == "Retry-After" {
			if n, err := strconv.Atoi(got); err != nil || n < 1 || n > 60 {
				t.Errorf("Retry-After %q", got)
			}
			continue
		}
		if got != v {
			t.Errorf("%s = %q, want %q", k, got, v)
		}
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("Content-Type %q", ct)
	}
	var got, want any
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("body %q: %v", rec.Body.String(), err)
	}
	_ = json.Unmarshal(c.Response.Body, &want)
	g, _ := json.Marshal(got)
	w, _ := json.Marshal(want)
	if !bytes.Equal(g, w) {
		t.Errorf("body\n got %s\nwant %s", g, w)
	}
	return rec
}

func TestContractFixtures(t *testing.T) {
	// The model fixture case that must answer each public case's model call.
	wantModelCase := map[string]string{
		"recommend/ok":            "ok",
		"recommend/ok_cold_start": "ok_cold_start_pop",
		"compare/ok":              "ok",
		"recommend/ok_genres":     "ok_genres",
		"compare/ok_genres":       "ok_genres",
		"recommend/ok_years":      "ok_years",
		"compare/ok_years":        "ok_years",
	}
	for _, op := range []string{"recommend", "compare"} {
		for _, c := range cachetest.PublicCases(t, op) {
			t.Run(op+"/"+c.Name, func(t *testing.T) {
				e := newEnv(t, nil)
				uid, tok := cachetest.NewUser(t, e.DB, "alice")
				if strings.HasSuffix(c.Request.Headers["Cookie"], "-bob") {
					bobID, bobTok := cachetest.NewUser(t, e.DB, "bob")
					seedBob(t, e.DB, bobID)
					c.Request.Headers["Cookie"] = "session=" + bobTok
				} else {
					seedAlice(t, e.DB, uid)
				}
				switch c.Name {
				case "model_unavailable":
					e.Fake.SetDown(true)
				case "rate_limited":
					for i := 0; i < 30; i++ {
						if rec := get(e, "/api/recommend?model=als&k=3", tok); rec.Code != 200 {
							t.Fatalf("request %d: %d %s", i, rec.Code, rec.Body.String())
						}
					}
				}
				e.Fake.ResetCalls()
				replay(t, e, c, tok)

				if want, ok := wantModelCase[op+"/"+c.Name]; ok {
					opID := fake.OpRecommend
					if op == "compare" {
						opID = fake.OpCompare
					}
					found := false
					for _, call := range e.Fake.Calls() {
						if call.OperationID == opID {
							found = true
							if call.Case != want {
								t.Errorf("model call answered by case %q, want %q (body %s)", call.Case, want, call.Body)
							}
						}
					}
					if !found {
						t.Errorf("no %s call", opID)
					}
				}
			})
		}
	}
}

// ---------------------------------------------------------------- validation

func TestValidation(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	cases := []struct {
		path, code string
	}{
		{"/api/recommend?model=svd", "unknown_model"},
		{"/api/recommend?model=", "unknown_model"},
		{"/api/recommend?model=ALS", "unknown_model"},
		{"/api/recommend?k=0", "bad_request"},
		{"/api/recommend?k=101", "bad_request"},
		{"/api/recommend?k=abc", "bad_request"},
		{"/api/recommend?k=-1", "bad_request"},
		{"/api/recommend?k=1&k=2", "bad_request"},
		{"/api/recommend/compare", "bad_request"},
		{"/api/recommend/compare?models=", "bad_request"},
		{"/api/recommend/compare?models=pop,pop", "bad_request"},
		{"/api/recommend/compare?models=pop,,knn", "bad_request"},
		{"/api/recommend/compare?models=POP", "bad_request"},
		{"/api/recommend/compare?models=pop,knn,als,mf,content,pop_count", "bad_request"},
		{"/api/recommend/compare?models=pop,svd", "unknown_model"},
		{"/api/recommend/compare?models=pop&k=500", "bad_request"},
		{"/api/recommend/compare?models=" + strings.Repeat("a", 65), "bad_request"},
	}
	for _, c := range cases {
		rec := get(e, c.path, tok)
		if rec.Code != 400 || cachetest.ErrorCode(rec) != c.code {
			t.Errorf("%s: %d %s, want 400 %s", c.path, rec.Code, rec.Body.String(), c.code)
		}
	}
	for _, c := range e.Fake.Calls() {
		if c.OperationID == fake.OpRecommend || c.OperationID == fake.OpCompare {
			t.Fatalf("invalid request reached recs-model: %+v", c)
		}
	}
}

// Without a model parameter the default comes from /v1/models (als), and k
// defaults to 20.
func TestDefaultModelAndK(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	rec := get(e, "/api/recommend", tok)
	if rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	var req modelclient.RecommendRequest
	for _, c := range e.Fake.Calls() {
		if c.OperationID == fake.OpRecommend {
			_ = json.Unmarshal(c.Body, &req)
		}
	}
	if req.Model != modelclient.ModelALS || req.K != 20 || len(req.Ratings) != 5 || len(req.Exclude) != 5 {
		t.Fatalf("request = %+v", req)
	}
	if !strings.Contains(rec.Header().Get("Cache-Control"), "no-store") {
		t.Errorf("Cache-Control %q", rec.Header().Get("Cache-Control"))
	}
}

// ---------------------------------------------------------------- cold start

func TestColdStartBelowFiveRatings(t *testing.T) {
	for n := 0; n <= 5; n++ {
		t.Run(strconv.Itoa(n), func(t *testing.T) {
			// A fresh server per path: a cold compare would otherwise be
			// answered from the cold recommend's cache entry (both are pop).
			for _, path := range []string{"/api/recommend?model=knn&k=3", "/api/recommend/compare?models=knn,als&k=3"} {
				e := newEnv(t, nil)
				uid, tok := cachetest.NewUser(t, e.DB, "user")
				for i := 0; i < n; i++ {
					rate(t, e.DB, uid, int64(i+1), 4, int64(i))
				}
				rec := get(e, path, tok)
				if rec.Code != 200 {
					t.Fatalf("%s: %d %s", path, rec.Code, rec.Body.String())
				}
				var body struct {
					Model     string `json:"model"`
					ColdStart bool   `json:"cold_start"`
					Results   []struct {
						Model string `json:"model"`
					} `json:"results"`
				}
				if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
					t.Fatal(err)
				}
				wantCold := n < 5
				if body.ColdStart != wantCold {
					t.Fatalf("%s with %d ratings: cold_start %v", path, n, body.ColdStart)
				}
				var sentModels []modelclient.ModelName
				for _, c := range e.Fake.Calls() {
					switch c.OperationID {
					case fake.OpRecommend:
						var r modelclient.RecommendRequest
						_ = json.Unmarshal(c.Body, &r)
						sentModels = append(sentModels, r.Model)
						if len(r.Ratings) != n {
							t.Errorf("sent %d ratings, want %d", len(r.Ratings), n)
						}
					case fake.OpCompare:
						var r modelclient.CompareRequest
						_ = json.Unmarshal(c.Body, &r)
						sentModels = append(sentModels, r.Models...)
					}
				}
				if wantCold {
					if len(sentModels) != 1 || sentModels[0] != modelclient.ModelPop {
						t.Errorf("%s cold start asked recs-model for %v, want [pop]", path, sentModels)
					}
					if strings.Contains(path, "compare") {
						if len(body.Results) != 1 || body.Results[0].Model != "pop" {
							t.Errorf("compare cold start results %+v", body.Results)
						}
					} else if body.Model != "pop" {
						t.Errorf("cold start model %q", body.Model)
					}
				} else if len(sentModels) == 0 || sentModels[0] == modelclient.ModelPop {
					t.Errorf("%s warm asked for %v", path, sentModels)
				}
			}
		})
	}
}

// ---------------------------------------------------------------- cache

// versioned wraps the fake so the artifact version can change between calls:
// Version and every Meta report the current version, as a recs-model that
// hot-swapped its artifact would.
type versioned struct {
	*fake.Fake
	mu         sync.Mutex
	version    string
	recommends atomic.Int64
	compares   atomic.Int64
	versions   atomic.Int64
}

func (v *versioned) set(s string) {
	v.mu.Lock()
	v.version = s
	v.mu.Unlock()
}

func (v *versioned) cur() string {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.version
}

func (v *versioned) Version(ctx context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	v.versions.Add(1)
	if _, _, err := v.Fake.Version(ctx); err != nil {
		return modelclient.VersionResponse{}, modelclient.Meta{}, err
	}
	s := v.cur()
	return modelclient.VersionResponse{ArtifactVersion: s}, modelclient.Meta{ModelVersion: s}, nil
}

func (v *versioned) Recommend(ctx context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	v.recommends.Add(1)
	resp, _, err := v.Fake.Recommend(ctx, req)
	return resp, modelclient.Meta{ModelVersion: v.cur()}, err
}

func (v *versioned) Compare(ctx context.Context, req modelclient.CompareRequest) (modelclient.CompareResponse, modelclient.Meta, error) {
	v.compares.Add(1)
	resp, _, err := v.Fake.Compare(ctx, req)
	return resp, modelclient.Meta{ModelVersion: v.cur()}, err
}

func newVersioned(t *testing.T) *versioned {
	t.Helper()
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	return &versioned{Fake: f, version: "v1"}
}

func modelVersionOf(t *testing.T, rec *httptest.ResponseRecorder) string {
	t.Helper()
	var b struct {
		ModelVersion string `json:"model_version"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil {
		t.Fatal(err)
	}
	if h := rec.Header().Get("X-Model-Version"); h != b.ModelVersion {
		t.Fatalf("X-Model-Version %q but body model_version %q", h, b.ModelVersion)
	}
	return b.ModelVersion
}

// Gate: the cache never serves a response computed by an older model
// version.
func TestCacheNeverServesOlderModelVersion(t *testing.T) {
	m := newVersioned(t)
	e := newEnv(t, m)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)

	for _, p := range []struct {
		path  string
		calls *atomic.Int64
	}{{"/api/recommend?model=als&k=3", &m.recommends}, {"/api/recommend/compare?models=pop,knn,als&k=3", &m.compares}} {
		m.set("v1")
		p.calls.Store(0)
		m.versions.Store(0)

		if rec := get(e, p.path, tok); rec.Code != 200 || modelVersionOf(t, rec) != "v1" {
			t.Fatalf("%s first: %d %s", p.path, rec.Code, rec.Body.String())
		}
		if rec := get(e, p.path, tok); rec.Code != 200 || modelVersionOf(t, rec) != "v1" || p.calls.Load() != 1 {
			t.Fatalf("%s second (cached): %d calls %d", p.path, rec.Code, p.calls.Load())
		}
		m.set("v2")
		rec := get(e, p.path, tok)
		if rec.Code != 200 || modelVersionOf(t, rec) != "v2" {
			t.Fatalf("%s after swap: %d model_version %s", p.path, rec.Code, rec.Body.String())
		}
		if p.calls.Load() != 2 {
			t.Fatalf("%s after swap: recs-model called %d times, want 2 (cache served the old version)", p.path, p.calls.Load())
		}
		if m.versions.Load() != 3 {
			t.Fatalf("%s: /v1/version called %d times for 3 requests", p.path, m.versions.Load())
		}
		// Rolling back to v1 may reuse what v1 computed: it is still v1's answer.
		m.set("v1")
		if rec := get(e, p.path, tok); modelVersionOf(t, rec) != "v1" {
			t.Fatalf("%s after rollback: %s", p.path, rec.Body.String())
		}
	}
}

// A version swap between the version check and the recommend call: the
// response is labelled (and cached) with the version that computed it, so a
// later request at the new version can't receive an older answer.
func TestCacheKeyedOnComputingVersion(t *testing.T) {
	m := &swapDuring{versioned: newVersioned(t)}
	e := newEnv(t, m)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	// Version says v1, then recommend runs on v2.
	m.set("v1")
	m.swapTo = "v2"
	rec := get(e, "/api/recommend?model=als&k=3", tok)
	if rec.Code != 200 || modelVersionOf(t, rec) != "v2" {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	m.swapTo = ""
	// Model rolls back to v1: the v2 answer must not be served.
	m.set("v1")
	rec = get(e, "/api/recommend?model=als&k=3", tok)
	if modelVersionOf(t, rec) != "v1" || m.recommends.Load() != 2 {
		t.Fatalf("served %s after %d recommend calls", rec.Body.String(), m.recommends.Load())
	}
}

type swapDuring struct {
	*versioned
	swapTo string
}

func (s *swapDuring) Recommend(ctx context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	if s.swapTo != "" {
		s.set(s.swapTo)
	}
	return s.versioned.Recommend(ctx, req)
}

// A rating change (ratings_version bump), a different k or model, and a
// different user all miss the cache.
func TestCacheKey(t *testing.T) {
	m := newVersioned(t)
	e := newEnv(t, m)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	uid2, tok2 := cachetest.NewUser(t, e.DB, "carol")
	seedAlice(t, e.DB, uid2)

	steps := []struct {
		name, path, token string
		wantCalls         int64
		before            func()
	}{
		{"first", "/api/recommend?model=als&k=3", tok, 1, nil},
		{"same", "/api/recommend?model=als&k=3", tok, 1, nil},
		{"other k", "/api/recommend?model=als&k=4", tok, 2, nil},
		{"other model", "/api/recommend?model=knn&k=3", tok, 3, nil},
		{"default model = als", "/api/recommend?k=3", tok, 3, nil},
		{"other user", "/api/recommend?model=als&k=3", tok2, 4, nil},
		{"rating changed", "/api/recommend?model=als&k=3", tok, 5, func() { rate(t, e.DB, uid, 4993, 3, 1794256900) }},
		{"same again", "/api/recommend?model=als&k=3", tok, 5, nil},
	}
	for _, s := range steps {
		if s.before != nil {
			s.before()
		}
		if rec := get(e, s.path, s.token); rec.Code != 200 {
			t.Fatalf("%s: %d %s", s.name, rec.Code, rec.Body.String())
		}
		if got := m.recommends.Load(); got != s.wantCalls {
			t.Fatalf("%s: %d recommend calls, want %d", s.name, got, s.wantCalls)
		}
	}
}

func TestLRUBounded(t *testing.T) {
	c := newLRU(3, 1<<20, 100)
	for i := 0; i < 5; i++ {
		c.put(cacheKey{userID: int64(i)}, &entry{modelVersion: strconv.Itoa(i)})
	}
	if c.len() != 3 {
		t.Fatalf("len %d", c.len())
	}
	if _, ok := c.get(cacheKey{userID: 0}); ok {
		t.Fatal("oldest entry kept")
	}
	// Touch 2, insert 5: 3 is now least recently used.
	if _, ok := c.get(cacheKey{userID: 2}); !ok {
		t.Fatal("entry 2 missing")
	}
	c.put(cacheKey{userID: 5}, &entry{})
	if _, ok := c.get(cacheKey{userID: 3}); ok {
		t.Fatal("LRU order ignored")
	}
	if _, ok := c.get(cacheKey{userID: 2}); !ok {
		t.Fatal("recently used entry evicted")
	}
}

// ---------------------------------------------------------------- rate limit

// Gate: 30 per minute per session, then 429 with Retry-After; another
// session is unaffected, and compare shares the bucket.
func TestRateLimitPerSession(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	uid2, tok2 := cachetest.NewUser(t, e.DB, "carol")
	seedAlice(t, e.DB, uid2)
	for i := 0; i < 30; i++ {
		path := "/api/recommend?model=als&k=3"
		if i%2 == 1 {
			path = "/api/recommend/compare?models=pop,knn,als&k=3"
		}
		if rec := get(e, path, tok); rec.Code != 200 {
			t.Fatalf("request %d: %d %s", i+1, rec.Code, rec.Body.String())
		}
	}
	for _, path := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop&k=3"} {
		rec := get(e, path, tok)
		if rec.Code != 429 || cachetest.ErrorCode(rec) != "rate_limited" || rec.Header().Get("Retry-After") == "" {
			t.Fatalf("%s 31st: %d %s %v", path, rec.Code, rec.Body.String(), rec.Header())
		}
	}
	if rec := get(e, "/api/recommend?model=als&k=3", tok2); rec.Code != 200 {
		t.Fatalf("other session limited: %d", rec.Code)
	}
	if rec := get(e, "/api/recommend?model=als&k=3", ""); rec.Code != 401 {
		t.Fatalf("no session: %d", rec.Code)
	}
}

// ---------------------------------------------------------------- degradation

// Gate: with recs-model down or hung, both routes answer 503
// model_unavailable within 1 s (real HTTP client against the fake server).
func TestModelDownWithinOneSecond(t *testing.T) {
	srv, err := fake.NewServer()
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	client := modelclient.NewHTTP(modelclient.HTTPConfig{Addr: strings.TrimPrefix(srv.URL, "http://")})
	e := newEnv(t, client)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)

	if rec := get(e, "/api/recommend?model=als&k=3", tok); rec.Code != 200 {
		t.Fatalf("healthy: %d %s", rec.Code, rec.Body.String())
	}
	for _, mode := range []string{"down", "hung"} {
		srv.Fake.SetDown(mode == "down")
		if mode == "hung" {
			srv.Fake.SetLatency(5 * time.Second)
		}
		for _, path := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn,als&k=3"} {
			start := time.Now()
			rec := get(e, path, tok)
			took := time.Since(start)
			if rec.Code != 503 || cachetest.ErrorCode(rec) != "model_unavailable" {
				t.Fatalf("%s %s: %d %s", mode, path, rec.Code, rec.Body.String())
			}
			if took > time.Second {
				t.Fatalf("%s %s took %v", mode, path, took)
			}
		}
	}
}

// Every stage of a request (version, models, recommend, batch) failing maps
// to 503, and nothing half-built is cached.
func TestModelFailureAtEachStage(t *testing.T) {
	for _, op := range []string{fake.OpVersion, fake.OpModels, fake.OpRecommend, fake.OpMoviesBatch} {
		t.Run(op, func(t *testing.T) {
			e := newEnv(t, nil)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			seedAlice(t, e.DB, uid)
			if err := e.Fake.Force(op, "model_unavailable"); err != nil {
				t.Fatal(err)
			}
			rec := get(e, "/api/recommend?k=3", tok)
			if rec.Code != 503 || cachetest.ErrorCode(rec) != "model_unavailable" {
				t.Fatalf("%d %s", rec.Code, rec.Body.String())
			}
			_ = e.Fake.Force(op, "")
			if rec := get(e, "/api/recommend?k=3", tok); rec.Code != 200 {
				t.Fatalf("after recovery: %d %s", rec.Code, rec.Body.String())
			}
		})
	}
}

// ---------------------------------------------------------------- enrichment

// Items and because entries recs-model can't describe are dropped, because
// is capped at 3, and titles come from the movie cache.
func TestEnrichment(t *testing.T) {
	m := &stubModel{items: []modelclient.Item{
		{MovieID: 10, Score: 0.9, Because: []modelclient.Because{{MovieID: 1, Weight: 0.5}, {MovieID: 99, Weight: 0.4}, {MovieID: 2, Weight: 0.3}, {MovieID: 3, Weight: 0.2}, {MovieID: 4, Weight: 0.1}}},
		{MovieID: 98, Score: 0.8, Because: []modelclient.Because{}},
		{MovieID: 11, Score: 0.7, Because: nil},
	}}
	e := newEnv(t, m)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	for i := int64(1); i <= 5; i++ {
		rate(t, e.DB, uid, i, 4, i)
	}
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(1, "One", 2001), cachetest.Movie(2, "Two", 2002), cachetest.Movie(3, "Three", 2003),
		cachetest.Movie(4, "Four", 2004), cachetest.Movie(10, "Ten", 2010), cachetest.Movie(11, "Eleven", 2011))
	rec := get(e, "/api/recommend?model=knn&k=3", tok)
	if rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	want := `{"model":"knn","model_version":"v","items":[` +
		`{"movieId":10,"title":"Ten","year":2010,"score":0.9,"because":[{"movieId":1,"title":"One","weight":0.5},{"movieId":2,"title":"Two","weight":0.3},{"movieId":3,"title":"Three","weight":0.2}]},` +
		`{"movieId":11,"title":"Eleven","year":2011,"score":0.7,"because":[]}],"cold_start":false}`
	if strings.TrimSpace(rec.Body.String()) != want {
		t.Fatalf("body\n got %s\nwant %s", rec.Body.String(), want)
	}
}

// stubModel answers Version, Models and Recommend; MoviesBatch says every ID
// is missing (so only seeded cache rows describe movies).
type stubModel struct {
	modelclient.Client
	items []modelclient.Item
}

func (s *stubModel) Version(context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	return modelclient.VersionResponse{ArtifactVersion: "v"}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *stubModel) Models(context.Context) (modelclient.ModelsResponse, modelclient.Meta, error) {
	return modelclient.ModelsResponse{ArtifactVersion: "v", DefaultModel: modelclient.ModelKNN,
		Models: []modelclient.ModelName{modelclient.ModelPop, modelclient.ModelKNN}}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *stubModel) Recommend(_ context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	return modelclient.RecommendResponse{Model: req.Model, Items: s.items}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *stubModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	return modelclient.BatchResponse{Movies: []modelclient.MovieMeta{}, Missing: ids}, modelclient.Meta{ModelVersion: "v"}, nil
}

// A model that is in the enum but not loaded (recs-model says unknown_model)
// is a 400 unknown_model, not a 500.
func TestModelNotLoaded(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	if err := e.Fake.Force(fake.OpRecommend, "unknown_model"); err != nil {
		t.Fatal(err)
	}
	rec := get(e, "/api/recommend?model=hybrid&k=3", tok)
	if rec.Code != 400 || cachetest.ErrorCode(rec) != "unknown_model" {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
}

// Rating payloads never reach the logs.
func TestNoRatingsInLogs(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	_ = get(e, "/api/recommend?model=als&k=3", tok)
	e.Fake.SetDown(true)
	_ = get(e, "/api/recommend?model=knn&k=3", tok)
	logs := e.Logs.String()
	for _, s := range []string{"rating", "\"exclude\"", tok} {
		if strings.Contains(logs, s) {
			t.Fatalf("log contains %q: %s", s, logs)
		}
	}
}

// Register works with the production wiring (no options).
func TestRegisterSignature(t *testing.T) {
	var _ func(*http.ServeMux, *app.Deps) = Register
}
