package recommend

import (
	"context"
	"encoding/json"
	"net/url"
	"slices"
	"strings"
	"sync"
	"testing"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// The optional genres query parameter of /api/recommend and
// /api/recommend/compare (contracts/public.yaml, parameters.Genres).

// sentCalls decodes the genres field of every recommend/compare call the
// fake received: present reports whether the field was in the body at all.
type sentCall struct {
	op      string
	model   string   // recommend
	models  []string // compare
	genres  []string
	present bool
}

func sentCalls(t *testing.T, f *fake.Fake) []sentCall {
	t.Helper()
	var out []sentCall
	for _, c := range f.Calls() {
		if c.OperationID != fake.OpRecommend && c.OperationID != fake.OpCompare {
			continue
		}
		var raw map[string]json.RawMessage
		if err := json.Unmarshal(c.Body, &raw); err != nil {
			t.Fatalf("%s body %s: %v", c.OperationID, c.Body, err)
		}
		s := sentCall{op: c.OperationID}
		if g, ok := raw["genres"]; ok {
			s.present = true
			if err := json.Unmarshal(g, &s.genres); err != nil {
				t.Fatalf("genres %s is not a JSON array of strings: %v", g, err)
			}
		}
		if m, ok := raw["model"]; ok {
			_ = json.Unmarshal(m, &s.model)
		}
		if m, ok := raw["models"]; ok {
			_ = json.Unmarshal(m, &s.models)
		}
		out = append(out, s)
	}
	return out
}

// A valid list goes to recs-model as a JSON array, in the caller's order;
// an absent or empty parameter sends no genres at all.
func TestGenresForwarded(t *testing.T) {
	cases := []struct {
		query string
		want  []string // nil = no genres field
	}{
		{"", nil},
		{"&genres=", nil},
		{"&genres=Horror", []string{"Horror"}},
		{"&genres=Thriller,Horror", []string{"Thriller", "Horror"}},
		{"&genres=Sci-Fi,Film-Noir,IMAX", []string{"Sci-Fi", "Film-Noir", "IMAX"}},
		{"&genres=" + strings.Join(contractGenres, ","), contractGenres},
	}
	for _, c := range cases {
		for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn,als&k=3"} {
			e := newEnv(t, nil)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			seedAlice(t, e.DB, uid)
			rec := get(e, base+c.query, tok)
			if rec.Code != 200 {
				t.Fatalf("%s%s: %d %s", base, c.query, rec.Code, rec.Body.String())
			}
			calls := sentCalls(t, e.Fake)
			if len(calls) != 1 {
				t.Fatalf("%s%s: %d model calls", base, c.query, len(calls))
			}
			got := calls[0]
			if c.want == nil {
				if got.present {
					t.Errorf("%s%s: sent genres %v, want none", base, c.query, got.genres)
				}
				continue
			}
			if !got.present || !slices.Equal(got.genres, c.want) {
				t.Errorf("%s%s: sent genres %v (present %v), want %v", base, c.query, got.genres, got.present, c.want)
			}
			if strings.Contains(base, "compare") && strings.Join(got.models, ",") != "pop,knn,als" {
				t.Errorf("compare sent models %v", got.models)
			}
		}
	}
}

// contractGenres is the Genre enum of contracts/public.yaml, in order.
var contractGenres = []string{"Action", "Adventure", "Animation", "Children", "Comedy", "Crime", "Documentary", "Drama",
	"Fantasy", "Film-Noir", "Horror", "IMAX", "Musical", "Mystery", "Romance", "Sci-Fi", "Thriller", "War", "Western"}

// Every malformed genres parameter is 400 bad_request with the standard
// error shape, and recs-model is never asked.
func TestGenresValidation(t *testing.T) {
	bad := []string{
		"Horrror",                    // unknown
		"horror",                     // case-sensitive
		"HORROR",                     // case-sensitive
		"Horror,Horror",              // duplicate
		"Action,Horror,Action",       // duplicate, not adjacent
		"Horror,,",                   // empty elements
		"Horror,",                    // trailing comma
		",Horror",                    // leading comma
		",",                          // only a comma
		"Horror,,Action",             // empty element in the middle
		"Horror, Action",             // space
		" Horror",                    // leading space
		"Sci Fi",                     // space instead of hyphen
		"Sci_Fi",                     // bad character
		"Horror;Action",              // bad separator
		"Horror\x00",                 // NUL
		"Horrör",                     // non-ASCII
		"(no genres listed)",         // MovieLens placeholder, not in the enum
		strings.Repeat("Horror", 40), // over 200 characters
		strings.Join(append(slices.Clone(contractGenres), "Action"), ","),  // 20 names
		strings.Join(append(slices.Clone(contractGenres), "Horrors"), ","), // 20 names, one unknown
		strings.Repeat("A,", 100) + "A",                                    // many elements
	}
	var queries []string
	for _, g := range bad {
		queries = append(queries, "&genres="+url.QueryEscape(g))
	}
	// The parameter given twice is also malformed.
	queries = append(queries, "&genres=Horror&genres=Action", "&genres=Horror&genres=", "&genres=&genres=")
	// A fresh user every few requests keeps clear of the 30/min budget.
	for i := 0; i < len(queries); i += 10 {
		e := newEnv(t, nil)
		uid, tok := cachetest.NewUser(t, e.DB, "alice")
		seedAlice(t, e.DB, uid)
		for _, q := range queries[i:min(i+10, len(queries))] {
			checkBadGenres(t, e, tok, q)
		}
		if calls := sentCalls(t, e.Fake); len(calls) != 0 {
			t.Fatalf("invalid genres reached recs-model: %+v", calls)
		}
	}
}

func checkBadGenres(t *testing.T, e *cachetest.Env, tok, query string) {
	t.Helper()
	{
		for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn&k=3"} {
			path := base + query
			rec := get(e, path, tok)
			if rec.Code != 400 || cachetest.ErrorCode(rec) != "bad_request" {
				t.Errorf("%q: %d %s, want 400 bad_request", path, rec.Code, rec.Body.String())
				continue
			}
			var b struct {
				Error struct {
					Code    string `json:"code"`
					Message string `json:"message"`
				} `json:"error"`
			}
			if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil || b.Error.Message == "" || len(b.Error.Message) > 128 {
				t.Errorf("%q: error body %s", path, rec.Body.String())
			}
		}
	}
}

// Cold start (< 5 ratings) asks for popularity, filtered by the same genres,
// on both routes.
func TestGenresColdStart(t *testing.T) {
	for _, path := range []string{"/api/recommend?model=knn&k=3&genres=Comedy,Drama", "/api/recommend/compare?models=knn,als&k=3&genres=Comedy,Drama"} {
		e := newEnv(t, nil)
		uid, tok := cachetest.NewUser(t, e.DB, "bob")
		seedBob(t, e.DB, uid)
		rec := get(e, path, tok)
		if rec.Code != 200 {
			t.Fatalf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
		var body struct {
			ColdStart bool `json:"cold_start"`
		}
		_ = json.Unmarshal(rec.Body.Bytes(), &body)
		if !body.ColdStart {
			t.Fatalf("%s: not cold start: %s", path, rec.Body.String())
		}
		calls := sentCalls(t, e.Fake)
		if len(calls) != 1 || calls[0].op != fake.OpRecommend || calls[0].model != "pop" {
			t.Fatalf("%s: model calls %+v, want one pop recommend", path, calls)
		}
		if !slices.Equal(calls[0].genres, []string{"Comedy", "Drama"}) {
			t.Errorf("%s: cold start sent genres %v", path, calls[0].genres)
		}
	}
}

// genreModel answers with a list that depends on the genres it was sent, so
// a response served from the wrong cache entry shows up in the body. It
// counts recommend and compare calls.
type genreModel struct {
	modelclient.Client
	t     *testing.T
	mu    sync.Mutex
	calls int
}

// movieFor maps a genre set (sorted, comma-joined) to the one movie answered.
var movieFor = map[string]int64{"": 10, "Horror": 11, "Action,Horror": 12, "Comedy": 13}

func (g *genreModel) answer(genres []string) []modelclient.Item {
	g.mu.Lock()
	g.calls++
	g.mu.Unlock()
	key := slices.Sorted(slices.Values(genres))
	id, ok := movieFor[strings.Join(key, ",")]
	if !ok {
		id = 99
	}
	return []modelclient.Item{{MovieID: id, Score: 1, Because: []modelclient.Because{}}}
}

// genresOf reads the genres a request carries through its JSON encoding, so
// this double doesn't depend on the Go field's name or type.
func genresOf(t *testing.T, req any) []string {
	t.Helper()
	b, err := json.Marshal(req)
	if err != nil {
		t.Error(err)
		return nil
	}
	var v struct {
		Genres []string `json:"genres"`
	}
	_ = json.Unmarshal(b, &v)
	return v.Genres
}

func (g *genreModel) Version(context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	return modelclient.VersionResponse{ArtifactVersion: "v"}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (g *genreModel) Models(context.Context) (modelclient.ModelsResponse, modelclient.Meta, error) {
	return modelclient.ModelsResponse{ArtifactVersion: "v", DefaultModel: modelclient.ModelALS,
		Models: []modelclient.ModelName{modelclient.ModelPop, modelclient.ModelKNN, modelclient.ModelALS}}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (g *genreModel) Recommend(_ context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	return modelclient.RecommendResponse{Model: req.Model, Items: g.answer(genresOf(g.t, req))}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (g *genreModel) Compare(_ context.Context, req modelclient.CompareRequest) (modelclient.CompareResponse, modelclient.Meta, error) {
	items := g.answer(genresOf(g.t, req))
	var res []modelclient.CompareResult
	for _, m := range req.Models {
		res = append(res, modelclient.CompareResult{Model: m, Items: items})
	}
	return modelclient.CompareResponse{Results: res}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (g *genreModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	return modelclient.BatchResponse{Movies: []modelclient.MovieMeta{}, Missing: ids}, modelclient.Meta{ModelVersion: "v"}, nil
}

// firstMovies returns the first item's movieId of every list in a response.
func firstMovies(t *testing.T, body []byte) []int64 {
	t.Helper()
	var b struct {
		Items   []RecItem `json:"items"`
		Results []struct {
			Items []RecItem `json:"items"`
		} `json:"results"`
	}
	if err := json.Unmarshal(body, &b); err != nil {
		t.Fatal(err)
	}
	var out []int64
	lists := [][]RecItem{}
	if b.Results == nil {
		lists = append(lists, b.Items)
	}
	for _, r := range b.Results {
		lists = append(lists, r.Items)
	}
	for _, l := range lists {
		if len(l) != 1 {
			t.Fatalf("list of %d items: %s", len(l), body)
		}
		out = append(out, l[0].MovieID)
	}
	return out
}

// Gate: the recommendation cache keys on the genre set. A filtered request
// never gets an unfiltered (or differently filtered) cached list, in either
// order of arrival; the same set in another order may share an entry.
func TestGenresCacheKey(t *testing.T) {
	for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn&k=3"} {
		for _, unfilteredFirst := range []bool{true, false} {
			gm := &genreModel{t: t}
			e := newEnv(t, gm)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			seedAlice(t, e.DB, uid)
			cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2010), cachetest.Movie(11, "Eleven", 2011),
				cachetest.Movie(12, "Twelve", 2012), cachetest.Movie(13, "Thirteen", 2013))

			type step struct {
				query     string
				wantMovie int64
				wantCalls int
			}
			steps := []step{
				{"", 10, 1},
				{"&genres=Horror", 11, 2},
				{"&genres=Horror,Action", 12, 3},
				{"&genres=Action,Horror", 12, 3}, // same set: may share the entry
				{"&genres=Comedy", 13, 4},
				{"&genres=Horror", 11, 4},
				{"", 10, 4},
				{"&genres=", 10, 4}, // empty = unfiltered
			}
			if !unfilteredFirst {
				steps = []step{
					{"&genres=Horror", 11, 1},
					{"&genres=Action,Horror", 12, 2},
					{"", 10, 3},
					{"&genres=", 10, 3},
					{"&genres=Horror,Action", 12, 3},
					{"&genres=Horror", 11, 3},
				}
			}
			for i, s := range steps {
				rec := get(e, base+s.query, tok)
				if rec.Code != 200 {
					t.Fatalf("%s%s: %d %s", base, s.query, rec.Code, rec.Body.String())
				}
				for _, m := range firstMovies(t, rec.Body.Bytes()) {
					if m != s.wantMovie {
						t.Fatalf("step %d %s%s (unfiltered first %v): served movie %d, want %d (wrong cache entry)",
							i, base, s.query, unfilteredFirst, m, s.wantMovie)
					}
				}
				gm.mu.Lock()
				calls := gm.calls
				gm.mu.Unlock()
				if calls != s.wantCalls {
					t.Fatalf("step %d %s%s: %d model calls, want %d", i, base, s.query, calls, s.wantCalls)
				}
			}
		}
	}
}

// Gate: the per-user and per-session budget of 30/min covers filtered
// requests and malformed ones alike.
func TestGenresRateLimited(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	tok2 := newSession(t, e, uid)
	genres := []string{"Horror", "Comedy", "Action,Drama", "Sci-Fi", "Horrror"}
	for i := 0; i < 30; i++ {
		tok := tok
		if i%2 == 1 {
			tok = tok2
		}
		path := "/api/recommend?model=als&k=3&genres=" + genres[i%len(genres)]
		if i%3 == 0 {
			path = "/api/recommend/compare?models=pop,knn&k=3&genres=" + genres[i%len(genres)]
		}
		rec := get(e, path, tok)
		if want := 200; genres[i%len(genres)] == "Horrror" {
			want = 400
			if rec.Code != want {
				t.Fatalf("request %d %s: %d", i+1, path, rec.Code)
			}
		} else if rec.Code != want {
			t.Fatalf("request %d %s: %d %s", i+1, path, rec.Code, rec.Body.String())
		}
	}
	tok3 := newSession(t, e, uid)
	for _, path := range []string{"/api/recommend?model=als&k=3&genres=Western", "/api/recommend/compare?models=pop&k=3&genres=War"} {
		rec := get(e, path, tok3)
		if rec.Code != 429 || cachetest.ErrorCode(rec) != "rate_limited" || rec.Header().Get("Retry-After") == "" {
			t.Fatalf("%s 31st: %d %s", path, rec.Code, rec.Body.String())
		}
	}
}

// The genre list never reaches the logs above debug level (recs-api has no
// request logging of query strings).
func TestGenresNotLogged(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	_ = get(e, "/api/recommend?model=als&k=3&genres=Documentary,Western", tok)
	_ = get(e, "/api/recommend?model=als&k=3&genres=Documentary,Westernn", tok)
	e.Fake.SetDown(true)
	_ = get(e, "/api/recommend?model=knn&k=3&genres=Documentary,Western", tok)
	for _, line := range strings.Split(e.Logs.String(), "\n") {
		if strings.Contains(line, `"level":"DEBUG"`) {
			continue
		}
		if strings.Contains(line, "Documentary") || strings.Contains(line, "Western") {
			t.Fatalf("log line above debug contains the genre list: %s", line)
		}
	}
}
