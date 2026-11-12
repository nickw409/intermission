package fake

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/contractsdir"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Synthetic fixtures so the loader and matcher are tested even before the
// real contracts/fixtures/model files exist.
var synthetic = map[string]string{
	"recommend.json": `{"operationId":"recommend","cases":[
	  {"name":"ok",
	   "request":{"method":"POST","path":"/v1/recommend","query":{},"headers":{},
	              "body":{"model":"knn","ratings":[{"movie_id":1,"rating":4.5}],"k":2}},
	   "response":{"status":200,"headers":{"X-Model-Version":"v1"},
	               "body":{"model":"knn","items":[{"movie_id":2,"score":0.5,"because":[{"movie_id":1,"weight":1.0}]}],"ignored":0}}},
	  {"name":"unknown_model",
	   "request":{"method":"POST","path":"/v1/recommend",
	              "body":{"model":"nope","ratings":[],"k":2}},
	   "response":{"status":400,"headers":{"X-Model-Version":"v1"},
	               "body":{"error":{"code":"unknown_model","message":"unknown model"}}}},
	  {"name":"invalid_json",
	   "request":{"method":"POST","path":"/v1/recommend","rawBody":"{not json"},
	   "response":{"status":400,"headers":{"X-Model-Version":"v1"},
	               "body":{"error":{"code":"invalid_json","message":"bad json"}}}},
	  {"name":"unavailable",
	   "request":{"method":"POST","path":"/v1/recommend","body":{"model":"als","ratings":[],"k":1,"exclude":[9]}},
	   "response":{"status":503,"body":{"error":{"code":"model_unavailable","message":"no artifact"}}}}]}`,
	"similar.json": `{"operationId":"similar","cases":[
	  {"name":"ok","request":{"method":"GET","path":"/v1/similar/1","query":{"model":"knn","k":2}},
	   "response":{"status":200,"headers":{"X-Model-Version":"v1"},
	               "body":{"model":"knn","movie_id":1,"items":[{"movie_id":3,"similarity":0.9}]}}},
	  {"name":"not_found","request":{"method":"GET","path":"/v1/similar/999?model=knn&k=2"},
	   "response":{"status":404,"headers":{"X-Model-Version":"v1"},
	               "body":{"error":{"code":"not_found","message":"unknown movie"}}}}]}`,
	"healthz.json": `{"operationId":"healthz","cases":[
	  {"name":"ok","request":{"method":"GET","path":"/healthz"},"response":{"status":200,"body":{"status":"ok"}}},
	  {"name":"unavailable","request":{"method":"GET","path":"/healthz"},"response":{"status":503,"body":{"status":"unavailable"}}}]}`,
	"version.json": `{"operationId":"version","cases":[
	  {"name":"ok","request":{"method":"GET","path":"/v1/version"},
	   "response":{"status":200,"headers":{"X-Model-Version":"v1"},"body":{"artifact_version":"v1"}}},
	  {"name":"unavailable","request":{"method":"GET","path":"/v1/version"},
	   "response":{"status":503,"headers":{"X-Model-Version":"none"},"body":{"error":{"code":"model_unavailable","message":"no artifact"}}}}]}`,
	"metrics.json": `{"operationId":"metrics","cases":[
	  {"name":"ok","request":{"method":"GET","path":"/metrics"},
	   "response":{"status":200,"headers":{"Content-Type":"text/plain; version=0.0.4"},"body":"recs_up 1\n"}}]}`,
}

func syntheticFake(t *testing.T) *Fake {
	t.Helper()
	dir := t.TempDir()
	for name, body := range synthetic {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	f, err := NewFromDir(dir)
	if err != nil {
		t.Fatalf("NewFromDir: %v", err)
	}
	return f
}

func TestMatchingInMemory(t *testing.T) {
	f := syntheticFake(t)
	ctx := reqid.WithID(context.Background(), "rid-1")

	resp, meta, err := f.Recommend(ctx, modelclient.RecommendRequest{Model: "knn", Ratings: []modelclient.RatingIn{{MovieID: 1, Rating: 4.5}}, K: 2})
	if err != nil || meta.ModelVersion != "v1" || len(resp.Items) != 1 || resp.Items[0].Because[0].Weight != 1 {
		t.Fatalf("ok case: resp=%+v meta=%+v err=%v", resp, meta, err)
	}

	_, _, err = f.Recommend(ctx, modelclient.RecommendRequest{Model: "nope", Ratings: []modelclient.RatingIn{}, K: 2})
	var se *modelclient.StatusError
	if !errors.As(err, &se) || se.Status != 400 || se.Code != "unknown_model" {
		t.Fatalf("unknown_model case: err=%v", err)
	}

	// Unmatched request falls back to the first success case.
	if _, _, err := f.Recommend(ctx, modelclient.RecommendRequest{Model: "pop", K: 7}); err != nil {
		t.Fatalf("fallback: %v", err)
	}

	if _, _, err := f.Recommend(ctx, modelclient.RecommendRequest{Model: "als", Ratings: []modelclient.RatingIn{}, K: 1, Exclude: []int64{9}}); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("503 case: err=%v", err)
	}

	if _, _, err := f.Similar(ctx, 1, modelclient.ModelKNN, 2); err != nil {
		t.Fatalf("similar ok: %v", err)
	}
	if _, _, err := f.Similar(ctx, 999, modelclient.ModelKNN, 2); !errors.As(err, &se) || se.Status != 404 {
		t.Fatalf("similar not_found (query given inside path): err=%v", err)
	}
	if err := f.Healthz(ctx); err != nil {
		t.Fatalf("healthz: %v", err)
	}
	if v, meta, err := f.Version(ctx); err != nil || v.ArtifactVersion != "v1" || meta.ModelVersion != "v1" {
		t.Fatalf("version: %+v %+v %v", v, meta, err)
	}

	calls := f.Calls()
	if len(calls) != 8 || calls[0].RequestID != "rid-1" || calls[0].Case != "ok" || calls[2].Case != "ok" {
		t.Fatalf("calls = %+v", calls)
	}
	f.ResetCalls()
	if len(f.Calls()) != 0 {
		t.Fatal("ResetCalls did not clear")
	}

	// No fixture for this operation.
	if _, _, err := f.Models(ctx); err == nil || errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("missing fixture: err=%v", err)
	}
}

func TestForce(t *testing.T) {
	f := syntheticFake(t)
	ctx := context.Background()
	if err := f.Force(OpHealthz, "unavailable"); err != nil {
		t.Fatal(err)
	}
	if err := f.Healthz(ctx); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("forced healthz: err=%v", err)
	}
	if err := f.Force(OpHealthz, ""); err != nil {
		t.Fatal(err)
	}
	if err := f.Healthz(ctx); err != nil {
		t.Fatalf("unforced healthz: %v", err)
	}
	if err := f.Force(OpVersion, "unavailable"); err != nil {
		t.Fatal(err)
	}
	if _, meta, err := f.Version(ctx); !errors.Is(err, modelclient.ErrUnavailable) || meta.ModelVersion != modelclient.ModelVersionNone {
		t.Fatalf("forced version: meta=%+v err=%v", meta, err)
	}
	if err := f.Force(OpHealthz, "nope"); err == nil {
		t.Fatal("Force with unknown case: want error")
	}
	if err := f.Force(OpModels, "ok"); err == nil {
		t.Fatal("Force with unknown operation: want error")
	}
}

func TestDownAndLatency(t *testing.T) {
	f := syntheticFake(t)
	f.SetDown(true)
	if _, _, err := f.Similar(context.Background(), 1, "", 0); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("down: err=%v", err)
	}
	f.SetDown(false)

	f.SetLatency(time.Second)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	start := time.Now()
	if err := f.Healthz(ctx); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Fatalf("latency past deadline: err=%v", err)
	}
	if time.Since(start) > 500*time.Millisecond {
		t.Fatal("latency did not honour the context deadline")
	}
	f.SetLatency(10 * time.Millisecond)
	start = time.Now()
	if err := f.Healthz(context.Background()); err != nil {
		t.Fatal(err)
	}
	if time.Since(start) < 10*time.Millisecond {
		t.Fatal("latency not applied")
	}
}

func TestServer(t *testing.T) {
	srv := NewServerFor(syntheticFake(t))
	defer srv.Close()

	post := func(body string, hdr map[string]string) *http.Response {
		req, _ := http.NewRequest("POST", srv.URL+"/v1/recommend", strings.NewReader(body))
		for k, v := range hdr {
			req.Header.Set(k, v)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		return resp
	}

	// Key order and whitespace do not matter; X-Request-ID is echoed.
	resp := post(`{ "k": 2, "ratings": [{"rating": 4.5, "movie_id": 1}], "model": "knn" }`, map[string]string{"X-Request-ID": "abc-123"})
	b, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || resp.Header.Get("X-Model-Version") != "v1" || resp.Header.Get("X-Request-ID") != "abc-123" {
		t.Fatalf("ok: status=%d headers=%v body=%s", resp.StatusCode, resp.Header, b)
	}
	var rr modelclient.RecommendResponse
	if err := json.Unmarshal(b, &rr); err != nil || rr.Model != "knn" {
		t.Fatalf("ok body: %s (%v)", b, err)
	}

	// Raw invalid JSON matches the rawBody case.
	resp = post(`{not json`, nil)
	b, _ = io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 400 || !bytes.Contains(b, []byte("invalid_json")) || resp.Header.Get("X-Request-ID") == "" {
		t.Fatalf("invalid_json: status=%d body=%s", resp.StatusCode, b)
	}

	// Text metrics are served raw.
	resp, err := http.Get(srv.URL + "/metrics")
	if err != nil {
		t.Fatal(err)
	}
	b, _ = io.ReadAll(resp.Body)
	resp.Body.Close()
	if string(b) != "recs_up 1\n" || !strings.HasPrefix(resp.Header.Get("Content-Type"), "text/plain") {
		t.Fatalf("metrics: %q %v", b, resp.Header)
	}

	// Unknown route.
	resp, _ = http.Get(srv.URL + "/v1/nope")
	resp.Body.Close()
	if resp.StatusCode != 404 {
		t.Fatalf("unknown route: %d", resp.StatusCode)
	}

	// Down.
	srv.Fake.SetDown(true)
	resp, _ = http.Get(srv.URL + "/healthz")
	b, _ = io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 503 || !bytes.Contains(b, []byte("model_unavailable")) {
		t.Fatalf("down: %d %s", resp.StatusCode, b)
	}
	if calls := srv.Fake.Calls(); calls[0].RequestID != "abc-123" {
		t.Fatalf("calls[0] = %+v", calls[0])
	}
}

func TestParseFixtureRejects(t *testing.T) {
	bad := map[string]string{
		"unknown op":    `{"operationId":"nope","cases":[{"name":"a","request":{"method":"GET","path":"/x"},"response":{"status":200}}]}`,
		"no cases":      `{"operationId":"models","cases":[]}`,
		"no name":       `{"operationId":"models","cases":[{"request":{"method":"GET","path":"/v1/models"},"response":{"status":200}}]}`,
		"dup name":      `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"/v1/models"},"response":{"status":200}},{"name":"a","request":{"method":"GET","path":"/v1/models"},"response":{"status":200}}]}`,
		"bad status":    `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"/v1/models"},"response":{"status":0}}]}`,
		"relative path": `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"v1/models"},"response":{"status":200}}]}`,
		"unknown field": `{"operationId":"models","extra":1,"cases":[{"name":"a","request":{"method":"GET","path":"/v1/models"},"response":{"status":200}}]}`,
		"object query":  `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"/v1/models","query":{"a":{}}},"response":{"status":200}}]}`,
		"trailing data": `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"/v1/models"},"response":{"status":200}}]} {}`,
		"not json":      `nope`,
		"body and raw":  `{"operationId":"models","cases":[{"name":"a","request":{"method":"GET","path":"/v1/models","body":{},"rawBody":"x"},"response":{"status":200}}]}`,
	}
	for name, in := range bad {
		if _, err := ParseFixture([]byte(in)); err == nil {
			t.Errorf("%s: want error", name)
		}
	}
}

func TestLoadDirFilenameMustMatch(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "wrong.json"), []byte(synthetic["healthz.json"]), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadDir(dir); err == nil {
		t.Fatal("want error for misnamed fixture")
	}
}

func FuzzParseFixture(f *testing.F) {
	for _, s := range synthetic {
		f.Add([]byte(s))
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		fx, err := ParseFixture(data)
		if err == nil && (fx == nil || len(fx.Cases) == 0) {
			t.Fatal("ParseFixture accepted a fixture without cases")
		}
	})
}

// ---- Real contract fixtures ----

// realFixtureDir returns contracts/fixtures/model, skipping the test when it
// does not exist yet or holds no fixtures.
func realFixtureDir(t *testing.T) string {
	t.Helper()
	dir, err := contractsdir.Fixtures("model")
	if err != nil {
		t.Fatalf("locate contracts: %v", err)
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.json"))
	if len(files) == 0 {
		t.Skipf("no model fixtures in %s yet; they are written in Wave 0 step S4", dir)
	}
	return dir
}

// invoke replays a fixture case through the in-memory Client. ok is false for
// operations or requests the typed client cannot express (metrics, raw
// invalid-JSON bodies).
func invoke(t *testing.T, f *Fake, opID string, c Case) (resp any, meta modelclient.Meta, err error, ok bool) {
	t.Helper()
	ctx := context.Background()
	body := bytes.TrimSpace(c.Request.Body)
	decode := func(v any) bool {
		if len(body) == 0 || body[0] != '{' {
			return false
		}
		return decodeStrict(body, v) == nil
	}
	q, qerr := queryStrings(c.Request.Query)
	if qerr != nil {
		t.Fatal(qerr)
	}
	atoi := func(s string) int {
		if s == "" {
			return 0
		}
		n, err := strconv.Atoi(s)
		if err != nil {
			t.Fatalf("%s/%s: non-integer query %q", opID, c.Name, s)
		}
		return n
	}
	switch opID {
	case OpRecommend:
		var r modelclient.RecommendRequest
		if !decode(&r) {
			return nil, meta, nil, false
		}
		resp, meta, err = f.Recommend(ctx, r)
	case OpCompare:
		var r modelclient.CompareRequest
		if !decode(&r) {
			return nil, meta, nil, false
		}
		resp, meta, err = f.Compare(ctx, r)
	case OpOnboarding:
		var r modelclient.OnboardingRequest
		if !decode(&r) {
			return nil, meta, nil, false
		}
		resp, meta, err = f.Onboarding(ctx, r)
	case OpMoviesBatch:
		var r modelclient.BatchRequest
		if !decode(&r) {
			return nil, meta, nil, false
		}
		resp, meta, err = f.MoviesBatch(ctx, r.IDs)
	case OpSimilar:
		id, perr := strconv.ParseInt(strings.TrimPrefix(c.Request.Path, "/v1/similar/"), 10, 64)
		if perr != nil {
			return nil, meta, nil, false
		}
		resp, meta, err = f.Similar(ctx, id, modelclient.ModelName(q["model"]), atoi(q["k"]))
	case OpSearch:
		resp, meta, err = f.Search(ctx, q["q"], atoi(q["limit"]))
	case OpModels:
		resp, meta, err = f.Models(ctx)
	case OpVersion:
		resp, meta, err = f.Version(ctx)
	case OpHealthz:
		err = f.Healthz(ctx)
		resp = modelclient.Health{Status: "ok"}
	default:
		return nil, meta, nil, false
	}
	return resp, meta, err, true
}

// checkOutcome asserts that a Client result corresponds to the case's response.
func checkOutcome(t *testing.T, opID string, c Case, resp any, meta modelclient.Meta, err error) {
	t.Helper()
	if want := c.Response.Header("X-Model-Version"); opID != OpHealthz && meta.ModelVersion != want {
		t.Errorf("%s/%s: ModelVersion = %q, want %q", opID, c.Name, meta.ModelVersion, want)
	}
	switch {
	case c.Response.Success():
		if err != nil {
			t.Errorf("%s/%s: err = %v", opID, c.Name, err)
			return
		}
		got, _ := json.Marshal(resp)
		if !jsonEqual(got, c.Response.Body) {
			t.Errorf("%s/%s: response round-trip mismatch\n got: %s\nwant: %s", opID, c.Name, got, c.Response.Body)
		}
	case c.Response.Status == 503:
		if !errors.Is(err, modelclient.ErrUnavailable) {
			t.Errorf("%s/%s: err = %v, want ErrUnavailable", opID, c.Name, err)
		}
	default:
		var se *modelclient.StatusError
		var eb modelclient.ErrorBody
		if jerr := decodeStrict(c.Response.Body, &eb); jerr != nil {
			t.Errorf("%s/%s: error body is not the Error shape: %v", opID, c.Name, jerr)
		}
		if !errors.As(err, &se) || se.Status != c.Response.Status || se.Code != eb.Error.Code {
			t.Errorf("%s/%s: err = %v, want StatusError %d %s", opID, c.Name, err, c.Response.Status, eb.Error.Code)
		}
	}
}

// Every fixture file loads, every operation has a success case, and every
// success case is what the fake returns for that case's own request (no
// Force), so request matching picks the right case.
func TestRealFixturesSuccess(t *testing.T) {
	dir := realFixtureDir(t)
	f, err := NewFromDir(dir)
	if err != nil {
		t.Fatalf("load %s: %v", dir, err)
	}
	fixtures := f.Fixtures()
	for _, opID := range OperationIDs() {
		if _, ok := fixtures[opID]; !ok && opID != OpMetrics {
			t.Errorf("no fixture for operation %s", opID)
		}
	}
	for opID, fx := range fixtures {
		n := 0
		for _, c := range fx.Cases {
			if !c.Response.Success() {
				continue
			}
			n++
			resp, meta, err, can := invoke(t, f, opID, c)
			if !can {
				continue
			}
			checkOutcome(t, opID, c, resp, meta, err)
		}
		if n == 0 {
			t.Errorf("%s: no success case", opID)
		}
	}
}

// Every case (success and error) decodes into the client types and maps to
// the right Go error, in memory and over HTTP.
func TestRealFixturesEveryCase(t *testing.T) {
	dir := realFixtureDir(t)
	f, err := NewFromDir(dir)
	if err != nil {
		t.Fatalf("load %s: %v", dir, err)
	}
	srv := NewServerFor(f)
	defer srv.Close()
	for opID, fx := range f.Fixtures() {
		for _, c := range fx.Cases {
			if err := f.Force(opID, c.Name); err != nil {
				t.Fatal(err)
			}
			if resp, meta, err, can := invoke(t, f, opID, c); can {
				checkOutcome(t, opID, c, resp, meta, err)
			}

			// Over HTTP: replay the fixture request verbatim.
			u := srv.URL + c.Request.Path
			if q, _ := queryStrings(c.Request.Query); len(q) > 0 {
				vals := make([]string, 0, len(q))
				for k, v := range q {
					vals = append(vals, k+"="+v)
				}
				u += "?" + strings.Join(vals, "&")
			}
			var body io.Reader
			if b := c.Request.BodyBytes(); len(b) > 0 {
				body = bytes.NewReader(b)
			}
			req, _ := http.NewRequest(c.Request.Method, u, body)
			hr, err := http.DefaultClient.Do(req)
			if err != nil {
				t.Fatalf("%s/%s: %v", opID, c.Name, err)
			}
			got, _ := io.ReadAll(hr.Body)
			hr.Body.Close()
			if hr.StatusCode != c.Response.Status {
				t.Errorf("%s/%s over HTTP: status %d, want %d", opID, c.Name, hr.StatusCode, c.Response.Status)
			}
			if opID != OpMetrics && !jsonEqual(got, c.Response.Body) {
				t.Errorf("%s/%s over HTTP: body %s, want %s", opID, c.Name, got, c.Response.Body)
			}
			if err := f.Force(opID, ""); err != nil {
				t.Fatal(err)
			}
		}
	}
}

// A movies/batch request no case matches is answered from the fixtures'
// catalog: known IDs in request order, the rest missing.
func TestMoviesBatchCatalogFallback(t *testing.T) {
	f, err := NewFromDir(realFixtureDir(t))
	if err != nil {
		t.Fatal(err)
	}
	resp, meta, err := f.MoviesBatch(context.Background(), []int64{593, 999999998, 480, 2959})
	if err != nil {
		t.Fatal(err)
	}
	var got []int64
	for _, m := range resp.Movies {
		got = append(got, m.MovieID)
	}
	if !slices.Equal(got, []int64{593, 480, 2959}) || !slices.Equal(resp.Missing, []int64{999999998}) {
		t.Fatalf("movies %v missing %v", got, resp.Missing)
	}
	if resp.Movies[0].Title != "Silence of the Lambs, The" || meta.ModelVersion == "" {
		t.Fatalf("%+v %+v", resp.Movies[0], meta)
	}
	if calls := f.Calls(); calls[len(calls)-1].Case != CatalogCase {
		t.Fatalf("answered by %q", calls[len(calls)-1].Case)
	}
	// An exact case still wins.
	if _, _, err := f.MoviesBatch(context.Background(), []int64{2571}); err != nil {
		t.Fatal(err)
	}
	if calls := f.Calls(); calls[len(calls)-1].Case != "movie_2571" {
		t.Fatalf("answered by %q", calls[len(calls)-1].Case)
	}
}

// A movies/batch request no case matches is checked as recs-model checks it
// (model-service batch_route, contracts/model.yaml BatchRequest): 1-200 IDs,
// each in [1, 2147483647], else 400.
func TestMoviesBatchFallbackRejectsInvalid(t *testing.T) {
	f, err := NewFromDir(realFixtureDir(t))
	if err != nil {
		t.Fatal(err)
	}
	tooMany := make([]int64, modelclient.MaxBatchIDs+1)
	for i := range tooMany {
		tooMany[i] = int64(i + 1)
	}
	for _, tc := range []struct {
		name string
		ids  []int64
		msg  string
	}{
		{"zero", []int64{260, 0}, "movie_id must be an integer between 1 and 2147483647"},
		{"negative", []int64{-5}, "movie_id must be an integer between 1 and 2147483647"},
		{"above int32", []int64{260, 2147483648}, "movie_id must be an integer between 1 and 2147483647"},
		{"over 200", tooMany, "ids must contain 1-200 movie ids"},
		{"nil", nil, "ids must contain 1-200 movie ids"},
	} {
		_, _, err := f.MoviesBatch(context.Background(), tc.ids)
		var se *modelclient.StatusError
		if !errors.As(err, &se) || se.Status != 400 || se.Code != "bad_request" || se.Message != tc.msg {
			t.Errorf("%s: err %v, want 400 bad_request %q", tc.name, err, tc.msg)
		}
	}
	// Exactly 200 valid IDs is fine (all unknown to the fixtures but one).
	ok := make([]int64, modelclient.MaxBatchIDs)
	for i := range ok {
		ok[i] = int64(i + 1)
	}
	resp, _, err := f.MoviesBatch(context.Background(), ok)
	if err != nil || len(resp.Movies)+len(resp.Missing) != modelclient.MaxBatchIDs {
		t.Fatalf("200 IDs: %v, %d movies, %d missing", err, len(resp.Movies), len(resp.Missing))
	}
	if _, _, err := f.MoviesBatch(context.Background(), []int64{2147483647}); err != nil {
		t.Fatalf("max ID: %v", err)
	}

	// Over HTTP, including bodies the typed client cannot send.
	srv := NewServerFor(f)
	defer srv.Close()
	const (
		badJSON  = "request body is not valid JSON"
		badCount = "ids must contain 1-200 movie ids"
		badID    = "movie_id must be an integer between 1 and 2147483647"
	)
	longKey := strings.Repeat("k", 41)
	for _, tc := range []struct{ body, code, msg string }{
		{`{"ids":[-5]}`, "bad_request", badID},
		{`{"ids":[2147483648]}`, "bad_request", badID},
		{`{"ids":[260,0]}`, "bad_request", badID},
		{`{"ids":[1.5]}`, "bad_request", badID},
		{`{"ids":["260"]}`, "bad_request", badID},
		{`{"ids":[[[[[[[1]]]]]]]}`, "bad_request", badID}, // depth 8: parsed, then rejected
		{`{"ids":null}`, "bad_request", badCount},
		{`{"ids":260}`, "bad_request", badCount},
		{`{"ids":[]}`, "bad_request", badCount}, // the empty_ids case
		{`{}`, "bad_request", `missing field "ids"`},
		{`{"ids":[260],"extra":1}`, "bad_request", `unexpected field "extra"`},
		{`{"zz":1,"ids":[260],"aa":1}`, "bad_request", `unexpected field "aa"`},
		{`{"ids":[260],"` + longKey + `":1}`, "bad_request", "unexpected field"},
		{`{"ids":[260],"caf\u00e9":1}`, "bad_request", "unexpected field"},
		{`[260]`, "bad_request", "request body must be a JSON object"},
		{`{"ids":[260]`, "invalid_json", badJSON},
		{`{"ids":[260]} {}`, "invalid_json", badJSON},
		{"{\"ids\":[260],\"\xff\":1}", "invalid_json", badJSON},
		{`{"ids":[[[[[[[[1]]]]]]]]}`, "invalid_json", badJSON}, // depth 9
		{`]{"ids":[260]}`, "invalid_json", badJSON},
	} {
		resp, err := http.Post(srv.URL+"/v1/movies/batch", "application/json", strings.NewReader(tc.body))
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		var eb modelclient.ErrorBody
		if resp.StatusCode != 400 || json.Unmarshal(b, &eb) != nil || eb.Error.Code != tc.code || eb.Error.Message != tc.msg {
			t.Errorf("%q: %d %s, want 400 %s %q", tc.body, resp.StatusCode, b, tc.code, tc.msg)
		}
	}

	// An integral float is an integer, as in JSON Schema and recs-model.
	hr, err := http.Post(srv.URL+"/v1/movies/batch", "application/json", strings.NewReader(`{"ids":[260.0, 999999998]}`))
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(hr.Body)
	hr.Body.Close()
	var br modelclient.BatchResponse
	if hr.StatusCode != 200 || json.Unmarshal(b, &br) != nil || len(br.Movies) != 1 || br.Movies[0].MovieID != 260 ||
		!slices.Equal(br.Missing, []int64{999999998}) {
		t.Errorf("260.0: %d %s", hr.StatusCode, b)
	}
}

// The catalog fallback merges every 2xx moviesBatch case, so the cases must
// agree on each movie's metadata, and no case may list a known movie as
// missing.
func TestMoviesBatchFixturesAgreePerMovie(t *testing.T) {
	fx, err := LoadDir(realFixtureDir(t))
	if err != nil {
		t.Fatal(err)
	}
	batch, ok := fx[OpMoviesBatch]
	if !ok {
		t.Fatal("no moviesBatch fixture")
	}
	type seen struct {
		meta json.RawMessage
		in   string
	}
	known := map[int64]seen{}
	missing := map[int64]string{}
	for _, c := range batch.Cases {
		if !c.Response.Success() {
			continue
		}
		var resp struct {
			Movies  []json.RawMessage `json:"movies"`
			Missing []int64           `json:"missing"`
		}
		if err := json.Unmarshal(c.Response.Body, &resp); err != nil {
			t.Fatalf("%s: %v", c.Name, err)
		}
		for _, raw := range resp.Movies {
			var m modelclient.MovieMeta
			if err := decodeStrict(raw, &m); err != nil {
				t.Fatalf("%s: %v", c.Name, err)
			}
			if prev, dup := known[m.MovieID]; dup && !jsonEqual(prev.meta, raw) {
				t.Errorf("movie %d: %s says %s, %s says %s", m.MovieID, prev.in, prev.meta, c.Name, raw)
			} else if !dup {
				known[m.MovieID] = seen{raw, c.Name}
			}
		}
		for _, id := range resp.Missing {
			missing[id] = c.Name
		}
	}
	if len(known) == 0 {
		t.Fatal("no movies in the moviesBatch fixture")
	}
	for id, name := range missing {
		if k, ok := known[id]; ok {
			t.Errorf("movie %d: missing in %s but known in %s", id, name, k.in)
		}
	}
}
