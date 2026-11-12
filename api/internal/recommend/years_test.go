package recommend

import (
	"context"
	"encoding/json"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// The optional year_from and year_to query parameters of /api/recommend and
// /api/recommend/compare (contracts/public.yaml, parameters.YearFrom and
// parameters.YearTo).

// yearMessage is the one 400 message for every malformed year parameter
// (contracts/fixtures/public, inverted_years).
const yearMessage = "year_from and year_to must be whole years from 1800 to 2100, with year_from no later than year_to"

// sentYears is the year range of one recommend/compare call the fake
// received, read from the JSON body. A nil pointer means the field was not
// in the body at all.
type sentYears struct {
	op       string
	model    string
	from, to *int
	genres   []string
}

func yearCalls(t *testing.T, f *fake.Fake) []sentYears {
	t.Helper()
	var out []sentYears
	for _, c := range f.Calls() {
		if c.OperationID != fake.OpRecommend && c.OperationID != fake.OpCompare {
			continue
		}
		var raw map[string]json.RawMessage
		if err := json.Unmarshal(c.Body, &raw); err != nil {
			t.Fatalf("%s body %s: %v", c.OperationID, c.Body, err)
		}
		s := sentYears{op: c.OperationID}
		for name, dst := range map[string]**int{"year_from": &s.from, "year_to": &s.to} {
			v, ok := raw[name]
			if !ok {
				continue
			}
			var n int
			if err := json.Unmarshal(v, &n); err != nil {
				t.Fatalf("%s %s is not a JSON integer: %v", name, v, err)
			}
			*dst = &n
		}
		if m, ok := raw["model"]; ok {
			_ = json.Unmarshal(m, &s.model)
		}
		if g, ok := raw["genres"]; ok {
			_ = json.Unmarshal(g, &s.genres)
		}
		out = append(out, s)
	}
	return out
}

func ip(n int) *int { return &n }

func eqYear(a, b *int) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

func fmtYear(p *int) string {
	if p == nil {
		return "absent"
	}
	return strconv.Itoa(*p)
}

// A valid bound goes to recs-model as a JSON integer; an absent one is left
// out of the body entirely. Either bound may be given alone.
func TestYearsForwarded(t *testing.T) {
	cases := []struct {
		query    string
		from, to *int
	}{
		{"", nil, nil},
		{"&year_from=1990&year_to=1999", ip(1990), ip(1999)},
		{"&year_from=1990", ip(1990), nil},
		{"&year_to=1999", nil, ip(1999)},
		{"&year_from=1995&year_to=1995", ip(1995), ip(1995)},
		{"&year_from=1800&year_to=2100", ip(1800), ip(2100)},
		{"&year_to=1800", nil, ip(1800)},
		{"&year_from=2100", ip(2100), nil},
		{"&year_to=2010&year_from=2000", ip(2000), ip(2010)},
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
			calls := yearCalls(t, e.Fake)
			if len(calls) != 1 {
				t.Fatalf("%s%s: %d model calls", base, c.query, len(calls))
			}
			if !eqYear(calls[0].from, c.from) || !eqYear(calls[0].to, c.to) {
				t.Errorf("%s%s: sent year_from %s year_to %s, want %s %s", base, c.query,
					fmtYear(calls[0].from), fmtYear(calls[0].to), fmtYear(c.from), fmtYear(c.to))
			}
		}
	}
}

// The range combines with the genre filter: both reach recs-model.
func TestYearsWithGenres(t *testing.T) {
	for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn&k=3"} {
		e := newEnv(t, nil)
		uid, tok := cachetest.NewUser(t, e.DB, "alice")
		seedAlice(t, e.DB, uid)
		path := base + "&genres=Horror,Thriller&year_from=1980&year_to=1989"
		rec := get(e, path, tok)
		if rec.Code != 200 {
			t.Fatalf("%s: %d %s", path, rec.Code, rec.Body.String())
		}
		calls := yearCalls(t, e.Fake)
		if len(calls) != 1 {
			t.Fatalf("%s: %d model calls", path, len(calls))
		}
		got := calls[0]
		if !eqYear(got.from, ip(1980)) || !eqYear(got.to, ip(1989)) || strings.Join(got.genres, ",") != "Horror,Thriller" {
			t.Errorf("%s: sent years %s..%s genres %v", path, fmtYear(got.from), fmtYear(got.to), got.genres)
		}
	}
}

// Every malformed year parameter is 400 bad_request with the one fixed
// message, on both routes, and recs-model is never asked.
func TestYearsValidation(t *testing.T) {
	badValues := []string{
		"",                           // empty
		"abc",                        // not a number
		"199a",                       // trailing letter
		"-1990",                      // sign
		"+1990",                      // leading plus
		" 1990",                      // leading space
		"1990 ",                      // trailing space
		"1990\x00",                   // NUL
		"1990.0",                     // decimal
		"1990.5",                     // fraction
		"1e3",                        // exponent
		"0x7C6",                      // hex
		"1,990",                      // separator
		"١٩٩٠",                       // non-ASCII digits
		"1799",                       // below range
		"2101",                       // above range
		"0",                          // far below
		"9999",                       // far above
		"01990",                      // leading zero, five characters
		"019900",                     // long
		"99999999999999999999999999", // overflows int
		strings.Repeat("1", 300),     // very long
	}
	var queries []string
	for _, v := range badValues {
		esc := url.QueryEscape(v)
		queries = append(queries, "&year_from="+esc, "&year_to="+esc)
		// A bad value next to a good other bound is still rejected.
		queries = append(queries, "&year_from="+esc+"&year_to=2000", "&year_from=1990&year_to="+esc)
	}
	queries = append(queries,
		"&year_from=2000&year_to=1990", // inverted
		"&year_from=2100&year_to=1800", // inverted, extremes
		"&year_from=1991&year_to=1990", // inverted by one
		"&year_from=1990&year_from=1990",
		"&year_from=1990&year_from=1991",
		"&year_to=1999&year_to=1999",
		"&year_from=1990&year_from=",
		"&year_to=&year_to=",
	)
	// A fresh user every few requests keeps clear of the 30/min budget
	// (each query is sent to both routes).
	for i := 0; i < len(queries); i += 10 {
		e := newEnv(t, nil)
		uid, tok := cachetest.NewUser(t, e.DB, "alice")
		seedAlice(t, e.DB, uid)
		for _, q := range queries[i:min(i+10, len(queries))] {
			checkBadYears(t, e, tok, q)
		}
		if calls := yearCalls(t, e.Fake); len(calls) != 0 {
			t.Fatalf("invalid years reached recs-model: %+v", calls)
		}
	}
}

func checkBadYears(t *testing.T, e *cachetest.Env, tok, query string) {
	t.Helper()
	for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn&k=3"} {
		path := base + query
		rec := get(e, path, tok)
		if rec.Code != 400 || cachetest.ErrorCode(rec) != "bad_request" {
			t.Errorf("%q: %d %s, want 400 bad_request", path, rec.Code, rec.Body.String())
			continue
		}
		var b struct {
			Error struct {
				Message string `json:"message"`
			} `json:"error"`
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil || b.Error.Message != yearMessage {
			t.Errorf("%q: error body %s, want message %q", path, rec.Body.String(), yearMessage)
		}
	}
}

// Cold start (< 5 ratings) asks for popularity with the same range (and
// genres), on both routes.
func TestYearsColdStart(t *testing.T) {
	cases := []struct {
		query    string
		from, to *int
	}{
		{"&year_from=1990&year_to=1999&genres=Comedy", ip(1990), ip(1999)},
		{"&year_from=2005", ip(2005), nil},
		{"&year_to=1970", nil, ip(1970)},
	}
	for _, c := range cases {
		for _, base := range []string{"/api/recommend?model=knn&k=3", "/api/recommend/compare?models=knn,als&k=3"} {
			path := base + c.query
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
			calls := yearCalls(t, e.Fake)
			if len(calls) != 1 || calls[0].op != fake.OpRecommend || calls[0].model != "pop" {
				t.Fatalf("%s: model calls %+v, want one pop recommend", path, calls)
			}
			if !eqYear(calls[0].from, c.from) || !eqYear(calls[0].to, c.to) {
				t.Errorf("%s: cold start sent years %s..%s", path, fmtYear(calls[0].from), fmtYear(calls[0].to))
			}
			if strings.Contains(c.query, "genres=Comedy") && strings.Join(calls[0].genres, ",") != "Comedy" {
				t.Errorf("%s: cold start sent genres %v", path, calls[0].genres)
			}
		}
	}
}

// yearModel answers with a list that depends on the year range it was sent,
// so a response served from the wrong cache entry shows up in the body. It
// counts recommend and compare calls.
type yearModel struct {
	modelclient.Client
	t     *testing.T
	mu    sync.Mutex
	calls int
}

// movieForYears maps a range ("from..to", "" for an absent bound) to the one
// movie answered.
var movieForYears = map[string]int64{
	"..":         20,
	"1990..1999": 21,
	"1990..":     22,
	"..1990":     23,
	"1990..1990": 24,
	"1980..1989": 25,
	"..1999":     26,
	"2000..2009": 27,
}

// yearsOf reads the range a request carries through its JSON encoding, so
// this double doesn't depend on the Go fields' names or types.
func yearsOf(t *testing.T, req any) string {
	t.Helper()
	b, err := json.Marshal(req)
	if err != nil {
		t.Error(err)
		return "?"
	}
	var v struct {
		From *int `json:"year_from"`
		To   *int `json:"year_to"`
	}
	_ = json.Unmarshal(b, &v)
	s := func(p *int) string {
		if p == nil {
			return ""
		}
		return strconv.Itoa(*p)
	}
	return s(v.From) + ".." + s(v.To)
}

func (y *yearModel) answer(rng string) []modelclient.Item {
	y.mu.Lock()
	y.calls++
	y.mu.Unlock()
	id, ok := movieForYears[rng]
	if !ok {
		id = 99
	}
	return []modelclient.Item{{MovieID: id, Score: 1, Because: []modelclient.Because{}}}
}

func (y *yearModel) Version(context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	return modelclient.VersionResponse{ArtifactVersion: "v"}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (y *yearModel) Models(context.Context) (modelclient.ModelsResponse, modelclient.Meta, error) {
	return modelclient.ModelsResponse{ArtifactVersion: "v", DefaultModel: modelclient.ModelALS,
		Models: []modelclient.ModelName{modelclient.ModelPop, modelclient.ModelKNN, modelclient.ModelALS}}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (y *yearModel) Recommend(_ context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	return modelclient.RecommendResponse{Model: req.Model, Items: y.answer(yearsOf(y.t, req))}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (y *yearModel) Compare(_ context.Context, req modelclient.CompareRequest) (modelclient.CompareResponse, modelclient.Meta, error) {
	items := y.answer(yearsOf(y.t, req))
	var res []modelclient.CompareResult
	for _, m := range req.Models {
		res = append(res, modelclient.CompareResult{Model: m, Items: items})
	}
	return modelclient.CompareResponse{Results: res}, modelclient.Meta{ModelVersion: "v"}, nil
}

func (y *yearModel) MoviesBatch(_ context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	return modelclient.BatchResponse{Movies: []modelclient.MovieMeta{}, Missing: ids}, modelclient.Meta{ModelVersion: "v"}, nil
}

// Gate: the recommendation cache keys on both bounds and tells an absent
// bound from a present one. A ranged request never gets an unranged (or
// differently ranged) cached list, in any order of arrival; the same range
// asked again is served from the cache.
func TestYearsCacheKey(t *testing.T) {
	type step struct {
		query     string
		wantMovie int64
		wantCalls int
	}
	orders := [][]step{
		{
			{"", 20, 1},
			{"&year_from=1990&year_to=1999", 21, 2},
			{"&year_from=1990", 22, 3},
			{"&year_to=1990", 23, 4},                // same number, other bound
			{"&year_from=1990&year_to=1990", 24, 5}, // both bounds, same number
			{"&year_to=1999", 26, 6},
			{"&year_from=1980&year_to=1989", 25, 7},
			{"&year_to=1999&year_from=1990", 21, 7}, // parameter order doesn't matter
			{"&year_from=1990", 22, 7},
			{"&year_to=1990", 23, 7},
			{"", 20, 7},
		},
		{
			{"&year_from=1990&year_to=1990", 24, 1},
			{"&year_to=1990", 23, 2},
			{"&year_from=1990", 22, 3},
			{"", 20, 4},
			{"&year_from=2000&year_to=2009", 27, 5},
			{"&year_from=1990&year_to=1990", 24, 5},
			{"&genres=Horror&year_from=1990&year_to=1999", 21, 6}, // genres also part of the key
			{"&year_from=1990&year_to=1999", 21, 7},
			{"&genres=Horror&year_from=1990&year_to=1999", 21, 7},
		},
	}
	for _, base := range []string{"/api/recommend?model=als&k=3", "/api/recommend/compare?models=pop,knn&k=3"} {
		for oi, steps := range orders {
			ym := &yearModel{t: t}
			e := newEnv(t, ym)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			seedAlice(t, e.DB, uid)
			var movies []modelclient.MovieMeta
			for _, id := range []int64{20, 21, 22, 23, 24, 25, 26, 27} {
				movies = append(movies, cachetest.Movie(id, "M"+strconv.FormatInt(id, 10), 2000))
			}
			cachetest.SeedMovies(t, e.DB, movies...)
			for i, s := range steps {
				rec := get(e, base+s.query, tok)
				if rec.Code != 200 {
					t.Fatalf("%s%s: %d %s", base, s.query, rec.Code, rec.Body.String())
				}
				for _, m := range firstMovies(t, rec.Body.Bytes()) {
					if m != s.wantMovie {
						t.Fatalf("order %d step %d %s%s: served movie %d, want %d (wrong cache entry)",
							oi, i, base, s.query, m, s.wantMovie)
					}
				}
				ym.mu.Lock()
				calls := ym.calls
				ym.mu.Unlock()
				if calls != s.wantCalls {
					t.Fatalf("order %d step %d %s%s: %d model calls, want %d", oi, i, base, s.query, calls, s.wantCalls)
				}
			}
		}
	}
}

// Gate: the bounds are part of cacheKey and of the entry's size estimate.
func TestYearsCacheKeyFields(t *testing.T) {
	base := cacheKey{userID: 1, op: "recommend", models: "als", k: 3}
	keys := []cacheKey{base}
	for _, r := range [][2]*int{{ip(1990), nil}, {nil, ip(1990)}, {ip(1990), ip(1990)}, {ip(1990), ip(1999)}, {ip(1991), ip(1999)}} {
		k := base
		k.years = yearsKey(r[0], r[1])
		keys = append(keys, k)
	}
	seen := map[cacheKey]int{}
	for i, k := range keys {
		if j, dup := seen[k]; dup {
			t.Errorf("ranges %d and %d share a cache key %+v", j, i, k)
		}
		seen[k] = i
	}
	e := &entry{}
	if e.size(keys[4]) <= e.size(keys[0]) {
		t.Errorf("entry size ignores the year component: %d vs %d", e.size(keys[4]), e.size(keys[0]))
	}
}

// The fixture's inverted_years case never reaches recs-model.
func TestYearsInvertedFixtureNoModelCall(t *testing.T) {
	for _, op := range []string{"recommend", "compare"} {
		for _, c := range cachetest.PublicCases(t, op) {
			if c.Name != "inverted_years" {
				continue
			}
			e := newEnv(t, nil)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			seedAlice(t, e.DB, uid)
			replay(t, e, c, tok)
			if calls := yearCalls(t, e.Fake); len(calls) != 0 {
				t.Errorf("%s/inverted_years reached recs-model: %+v", op, calls)
			}
		}
	}
}
