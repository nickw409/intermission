package movies

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

func newEnv(t *testing.T) *cachetest.Env {
	t.Helper()
	return cachetest.NewEnv(t, nil, Register)
}

func rate(t *testing.T, d *db.DB, uid, movie int64, rating float64, updated int64) {
	t.Helper()
	if _, err := d.Writer.Exec("INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)", uid, movie, rating, updated); err != nil {
		t.Fatal(err)
	}
}

// aliceRatings match the exclude list in contracts/fixtures/model/onboarding.json
// (newest first).
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

// ---------------------------------------------------------------- contract

func TestContractFixtures(t *testing.T) {
	for _, op := range []string{"searchMovies", "getMovie", "getSimilar", "getOnboarding", "listModels"} {
		for _, c := range cachetest.PublicCases(t, op) {
			t.Run(op+"/"+c.Name, func(t *testing.T) {
				e := newEnv(t)
				uid, tok := cachetest.NewUser(t, e.DB, "alice")
				switch {
				case op == "getMovie" && c.Name == "ok_rated_by_caller":
					rate(t, e.DB, uid, 2571, 4.5, 1)
				case op == "getOnboarding":
					seedAlice(t, e.DB, uid)
				}
				if c.Name == "model_unavailable" {
					e.Fake.SetDown(true)
				}
				rec := e.Replay(c, tok)
				if op == "getMovie" && rec.Code == 200 {
					if _, ok := c.Request.Headers["Cookie"]; ok && rec.Header().Get("Cache-Control") != "private, no-store" {
						t.Errorf("session response Cache-Control %q", rec.Header().Get("Cache-Control"))
					}
				}
			})
		}
	}
}

// ---------------------------------------------------------------- search

func TestSearchValidation(t *testing.T) {
	e := newEnv(t)
	long := strings.Repeat("a", 101)
	for _, q := range []string{"/api/movies/search", "/api/movies/search?q=", "/api/movies/search?q=" + long,
		"/api/movies/search?q=%ff%fe", "/api/movies/search?q=star&limit=0", "/api/movies/search?q=star&limit=51",
		"/api/movies/search?q=star&limit=abc", "/api/movies/search?q=star&limit=-1", "/api/movies/search?q=star&limit=1.5"} {
		rec := e.Do("GET", q, nil, "")
		if rec.Code != 400 || cachetest.ErrorCode(rec) != "bad_request" {
			t.Errorf("%s: %d %s", q, rec.Code, rec.Body.String())
		}
	}
	// 100 multi-byte characters is within the limit (characters, not bytes).
	q := strings.Repeat("é", 100)
	if rec := e.Do("GET", "/api/movies/search?q="+url.QueryEscape(q), nil, ""); rec.Code != 200 {
		t.Errorf("100 accented characters: %d %s", rec.Code, rec.Body.String())
	}
	if len(e.Fake.Calls()) == 0 || e.Fake.Calls()[0].Query.Get("q") != q {
		t.Errorf("calls = %+v", e.Fake.Calls())
	}
}

// SQL-injection payloads in the search query reach recs-model verbatim and
// touch no table.
func TestSearchPassesInjectionCorpusLiterally(t *testing.T) {
	e := newEnv(t)
	cachetest.NewUser(t, e.DB, "alice")
	corpus := []string{
		`' OR '1'='1`, `' OR 1=1--`, `" OR "1"="1`, `'; DROP TABLE movies; --`, `'; DELETE FROM ratings; --`,
		`' UNION SELECT username, pw_hash FROM users --`, `1; ATTACH DATABASE '/tmp/x.db' AS x; --`,
		`%' OR 1=1 --`, `') OR ('1'='1`, `'; PRAGMA writable_schema = 1; --`, `?; SELECT ?`, `$1 OR 1=1`,
		`x'00'`, `star&limit=50`, `star#frag`, `a+b c`, "tab\there", `\' OR 1=1 --`,
	}
	for _, p := range corpus {
		e.Fake.ResetCalls()
		rec := e.Do("GET", "/api/movies/search?q="+url.QueryEscape(p)+"&limit=3", nil, "")
		if rec.Code != 200 {
			t.Errorf("%q: %d %s", p, rec.Code, rec.Body.String())
			continue
		}
		calls := e.Fake.Calls()
		if len(calls) == 0 || calls[0].OperationID != fake.OpSearch || calls[0].Query.Get("q") != p || calls[0].Query.Get("limit") != "3" {
			t.Errorf("%q: forwarded as %+v", p, calls)
		}
	}
	var users, tables int
	_ = e.DB.Reader.QueryRow("SELECT count(*) FROM users").Scan(&users)
	_ = e.DB.Reader.QueryRow("SELECT count(*) FROM sqlite_master WHERE type = 'table'").Scan(&tables)
	if users != 1 || tables < 6 {
		t.Fatalf("users %d tables %d", users, tables)
	}
}

// Results keep recs-model's order and drop IDs it can't describe.
func TestSearchOrderAndUnknownIDs(t *testing.T) {
	m := &stubModel{search: []int64{3, 999, 1, 2}}
	e := cachetest.NewEnv(t, m, Register)
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(1, "One", 2001), cachetest.Movie(2, "Two", 2002), cachetest.Movie(3, "Three", 2003))
	rec := e.Do("GET", "/api/movies/search?q=x", nil, "")
	var got struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
		} `json:"items"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil || len(got.Items) != 3 ||
		got.Items[0].MovieID != 3 || got.Items[1].MovieID != 1 || got.Items[2].MovieID != 2 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if m.lastLimit != 0 {
		t.Fatalf("limit forwarded as %d with none given", m.lastLimit)
	}
}

// stubModel overrides a few operations; MoviesBatch says every ID is missing.
type stubModel struct {
	modelclient.Client
	search    []int64
	onboard   []int64
	lastLimit int
	lastOnb   modelclient.OnboardingRequest
}

func (s *stubModel) Search(_ context.Context, _ string, limit int) (modelclient.IDList, modelclient.Meta, error) {
	s.lastLimit = limit
	return modelclient.IDList{MovieIDs: s.search}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *stubModel) Onboarding(_ context.Context, req modelclient.OnboardingRequest) (modelclient.IDList, modelclient.Meta, error) {
	s.lastOnb = req
	return modelclient.IDList{MovieIDs: s.onboard}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (s *stubModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	return modelclient.BatchResponse{Movies: []modelclient.MovieMeta{}, Missing: ids}, modelclient.Meta{ModelVersion: "v"}, nil
}

// ---------------------------------------------------------------- movie

func TestGetMovieOptionalSession(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	rate(t, e.DB, uid, 4993, 3.5, 1)

	anon := e.Do("GET", "/api/movies/4993", nil, "")
	if anon.Code != 200 || !strings.Contains(anon.Body.String(), `"user_rating":null`) {
		t.Fatalf("anon: %d %s", anon.Code, anon.Body.String())
	}
	if strings.Contains(anon.Header().Get("Cache-Control"), "private") || anon.Header().Get("Vary") != "Cookie" {
		t.Fatalf("anon: Cache-Control %q Vary %q", anon.Header().Get("Cache-Control"), anon.Header().Get("Vary"))
	}
	me := e.Do("GET", "/api/movies/4993", nil, tok)
	if me.Code != 200 || !strings.Contains(me.Body.String(), `"user_rating":3.5`) || me.Header().Get("Cache-Control") != "private, no-store" {
		t.Fatalf("session: %d %s %v", me.Code, me.Body.String(), me.Header())
	}
	// A dead or malformed cookie is just anonymous: 200, no rating, no Set-Cookie.
	for _, bad := range []string{"not-a-token", strings.Repeat("A", 43)} {
		rec := e.Do("GET", "/api/movies/4993", nil, bad)
		if rec.Code != 200 || !strings.Contains(rec.Body.String(), `"user_rating":null`) || rec.Header().Get("Set-Cookie") != "" {
			t.Fatalf("bad cookie %q: %d %s %v", bad, rec.Code, rec.Body.String(), rec.Header())
		}
	}
}

// The movie page refreshes counts while recs-model is up and serves the
// cached row while it is down.
func TestGetMovieModelDownServesCache(t *testing.T) {
	e := newEnv(t)
	if rec := e.Do("GET", "/api/movies/4993", nil, ""); rec.Code != 200 {
		t.Fatal(rec.Code)
	}
	e.Fake.SetDown(true)
	rec := e.Do("GET", "/api/movies/4993", nil, "")
	if rec.Code != 200 || !strings.Contains(rec.Body.String(), `"rating_count":198`) {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if rec := e.Do("GET", "/api/movies/2571", nil, ""); rec.Code != 503 {
		t.Fatalf("uncached while down: %d", rec.Code)
	}
}

// ---------------------------------------------------------------- similar

func TestSimilarValidation(t *testing.T) {
	e := newEnv(t)
	for path, code := range map[string]string{
		"/api/movies/260/similar?model=als":  "unknown_model",
		"/api/movies/260/similar?model=KNN":  "unknown_model",
		"/api/movies/260/similar?model=":     "unknown_model",
		"/api/movies/260/similar?k=0":        "bad_request",
		"/api/movies/260/similar?k=101":      "bad_request",
		"/api/movies/260/similar?k=x":        "bad_request",
		"/api/movies/abc/similar":            "bad_request",
		"/api/movies/0/similar":              "bad_request",
		"/api/movies/2147483648/similar?k=3": "bad_request",
	} {
		rec := e.Do("GET", path, nil, "")
		if rec.Code != 400 || cachetest.ErrorCode(rec) != code {
			t.Errorf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
	}
	if len(e.Fake.Calls()) != 0 {
		t.Fatalf("invalid requests reached recs-model: %+v", e.Fake.Calls())
	}
	// Defaults: knn, k=20.
	e.Do("GET", "/api/movies/260/similar", nil, "")
	c := e.Fake.Calls()
	if len(c) == 0 || c[0].Query.Get("model") != "knn" || c[0].Query.Get("k") != "20" {
		t.Fatalf("defaults forwarded as %+v", c)
	}
}

// ---------------------------------------------------------------- onboarding

func TestOnboardingExcludesRated(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	// recs-model misbehaves and returns a rated movie (260) first.
	m := &stubModel{onboard: []int64{260, 356, 4993}}
	e2 := cachetest.NewEnv(t, m, Register)
	uid2, tok2 := cachetest.NewUser(t, e2.DB, "bob")
	rate(t, e2.DB, uid2, 260, 4, 2)
	rate(t, e2.DB, uid2, 1, 3, 1)
	cachetest.SeedMovies(t, e2.DB, cachetest.Movie(260, "SW", 1977), cachetest.Movie(356, "FG", 1994), cachetest.Movie(4993, "LOTR", 2001))
	rec := e2.Do("GET", "/api/onboarding?n=3", nil, tok2)
	if rec.Code != 200 || strings.Contains(rec.Body.String(), `"movieId":260`) || !strings.Contains(rec.Body.String(), `"movieId":356`) {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if m.lastOnb.N != 3 || !slices.Equal(m.lastOnb.Exclude, []int64{260, 1}) {
		t.Fatalf("onboarding request %+v", m.lastOnb)
	}
	// Default n.
	e2.Do("GET", "/api/onboarding", nil, tok2)
	if m.lastOnb.N != 30 {
		t.Fatalf("default n = %d", m.lastOnb.N)
	}
	for _, q := range []string{"n=0", "n=61", "n=x"} {
		if rec := e.Do("GET", "/api/onboarding?"+q, nil, tok); rec.Code != 400 {
			t.Errorf("%s: %d", q, rec.Code)
		}
	}
}

// Movies marked "not interested" are left out of onboarding as well, sent as
// exclude and filtered again.
func TestOnboardingExcludesNotInterested(t *testing.T) {
	m := &stubModel{onboard: []int64{2959, 356, 4993}}
	e := cachetest.NewEnv(t, m, Register)
	uid, tok := cachetest.NewUser(t, e.DB, "bob")
	rate(t, e.DB, uid, 260, 4, 2)
	for _, row := range []struct{ movie, ts int64 }{{2959, 5}, {260, 6}} { // 260 is also rated: listed once
		if _, err := e.DB.Writer.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, 'als', 'not_interested', ?)", uid, row.movie, row.ts); err != nil {
			t.Fatal(err)
		}
	}
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(2959, "FC", 1999), cachetest.Movie(356, "FG", 1994), cachetest.Movie(4993, "LOTR", 2001))
	rec := e.Do("GET", "/api/onboarding?n=3", nil, tok)
	if rec.Code != 200 || strings.Contains(rec.Body.String(), `"movieId":2959`) || !strings.Contains(rec.Body.String(), `"movieId":356`) {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if !slices.Equal(m.lastOnb.Exclude, []int64{260, 2959}) {
		t.Fatalf("exclude %v", m.lastOnb.Exclude)
	}
}

// ---------------------------------------------------------------- degradation

// Phase 2 gate: with recs-model hanging (not just refusing), every model
// route answers 503 model_unavailable within 1 s, through the real client.
func TestModelRoutesFailFastWhenModelHangs(t *testing.T) {
	f, err := fake.New()
	if err != nil {
		t.Fatal(err)
	}
	f.SetLatency(5 * time.Second)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: fake.Handler(f), ReadHeaderTimeout: time.Second}
	go func() { _ = srv.Serve(ln) }()
	t.Cleanup(func() { _ = srv.Close() })
	client := modelclient.NewHTTP(modelclient.HTTPConfig{Addr: ln.Addr().String()})

	e := cachetest.NewEnv(t, client, Register)
	_, tok := cachetest.NewUser(t, e.DB, "alice")
	for _, path := range []string{"/api/movies/search?q=star", "/api/movies/4993", "/api/movies/260/similar",
		"/api/onboarding", "/api/models"} {
		start := time.Now()
		rec := e.Do("GET", path, nil, tok)
		if d := time.Since(start); d > time.Second {
			t.Errorf("%s took %v", path, d)
		}
		if rec.Code != 503 || cachetest.ErrorCode(rec) != "model_unavailable" ||
			!strings.Contains(rec.Body.String(), "the recommendation service is unavailable") {
			t.Errorf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
	}
}
