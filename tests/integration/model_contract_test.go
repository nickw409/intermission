package integrationtest

// Gate N1, model side: recs-api's real model client (api/internal/modelclient)
// and raw HTTP against the real recs-model binary serving a tiny artifact.
// Every operation in contracts/model.yaml, its status codes, error shapes and
// headers (X-Model-Version on every response, X-Request-ID echoed).

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/reqid"
)

func modelReq(op, name, method, path string, body string) request {
	r := request{spec: "model", op: op, name: name, method: method, path: path,
		headers: map[string]string{"X-Request-ID": "it-" + strings.NewReplacer(" ", "-", "/", "-", ",", "").Replace(name)}}
	if body != "" {
		r.body = []byte(body)
	}
	return r
}

// someRatings are valid ratings for the tiny catalog (movie IDs 1-30).
const someRatings = `[{"movie_id":1,"rating":5},{"movie_id":2,"rating":4.5},{"movie_id":3,"rating":4},{"movie_id":7,"rating":3.5},{"movie_id":11,"rating":5}]`

func TestModelClientEveryOperation(t *testing.T) {
	c := modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: env.modelSock, Timeout: 2 * time.Second})
	ctx := reqid.WithID(context.Background(), "it-modelclient")
	ratings := []modelclient.RatingIn{{MovieID: 1, Rating: 5}, {MovieID: 2, Rating: 4.5}, {MovieID: 3, Rating: 4}, {MovieID: 7, Rating: 3.5}, {MovieID: 11, Rating: 5}, {MovieID: 999999, Rating: 3}}
	want := env.version
	checkMeta := func(t *testing.T, m modelclient.Meta) {
		t.Helper()
		if m.ModelVersion != want {
			t.Errorf("X-Model-Version = %q, want %q", m.ModelVersion, want)
		}
	}

	v, meta, err := c.Version(ctx)
	if err != nil || v.ArtifactVersion != want {
		t.Fatalf("Version = %+v, %v; want %s", v, err, want)
	}
	checkMeta(t, meta)

	models, meta, err := c.Models(ctx)
	if err != nil {
		t.Fatalf("Models: %v", err)
	}
	checkMeta(t, meta)
	if models.DefaultModel != modelclient.ModelKNN || len(models.Models) != 6 || models.ArtifactVersion != want {
		t.Errorf("Models = %+v", models)
	}

	for _, m := range models.Models {
		resp, meta, err := c.Recommend(ctx, modelclient.RecommendRequest{Model: m, Ratings: ratings, K: 5, Exclude: []int64{1, 2, 3, 7, 11, 424242}})
		if err != nil {
			t.Errorf("Recommend %s: %v", m, err)
			continue
		}
		checkMeta(t, meta)
		if resp.Model != m || resp.Ignored != 1 || len(resp.Items) == 0 || len(resp.Items) > 5 {
			t.Errorf("Recommend %s = %+v (want model %s, ignored 1, 1-5 items)", m, resp, m)
		}
		for _, it := range resp.Items {
			switch it.MovieID {
			case 1, 2, 3, 7, 11:
				t.Errorf("Recommend %s returned excluded movie %d", m, it.MovieID)
			}
			if len(it.Because) > 3 {
				t.Errorf("Recommend %s: %d because entries", m, len(it.Because))
			}
		}
	}

	cmp, meta, err := c.Compare(ctx, modelclient.CompareRequest{Models: []modelclient.ModelName{"als", "pop", "knn"}, Ratings: ratings, K: 3})
	if err != nil {
		t.Fatalf("Compare: %v", err)
	}
	checkMeta(t, meta)
	if len(cmp.Results) != 3 || cmp.Results[0].Model != "als" || cmp.Results[1].Model != "pop" || cmp.Results[2].Model != "knn" || cmp.Ignored != 1 {
		t.Errorf("Compare results not in request order or ignored wrong: %+v", cmp)
	}

	for _, m := range []modelclient.ModelName{"knn", "content", ""} {
		sim, meta, err := c.Similar(ctx, 1, m, 5)
		if err != nil {
			t.Errorf("Similar(%q): %v", m, err)
			continue
		}
		checkMeta(t, meta)
		wantModel := m
		if m == "" {
			wantModel = "knn"
		}
		if sim.Model != wantModel || sim.MovieID != 1 || len(sim.Items) > 5 {
			t.Errorf("Similar(%q) = %+v", m, sim)
		}
	}

	ob, meta, err := c.Onboarding(ctx, modelclient.OnboardingRequest{N: 10, Exclude: []int64{1}})
	if err != nil || len(ob.MovieIDs) == 0 || len(ob.MovieIDs) > 10 {
		t.Errorf("Onboarding = %+v, %v", ob, err)
	}
	checkMeta(t, meta)
	for _, id := range ob.MovieIDs {
		if id == 1 {
			t.Error("Onboarding returned an excluded movie")
		}
	}

	sr, meta, err := c.Search(ctx, "toy", 5)
	if err != nil || len(sr.MovieIDs) == 0 || sr.MovieIDs[0] != 1 {
		t.Errorf("Search(toy) = %+v, %v; want movie 1 first", sr, err)
	}
	checkMeta(t, meta)

	batch, meta, err := c.MoviesBatch(ctx, []int64{3, 1, 999999})
	if err != nil {
		t.Fatalf("MoviesBatch: %v", err)
	}
	checkMeta(t, meta)
	if len(batch.Movies) != 2 || batch.Movies[0].MovieID != 3 || batch.Movies[1].MovieID != 1 || len(batch.Missing) != 1 || batch.Missing[0] != 999999 {
		t.Errorf("MoviesBatch = %+v (want 3, 1 in request order, missing [999999])", batch)
	}

	if err := c.Healthz(ctx); err != nil {
		t.Errorf("Healthz: %v", err)
	}

	// Error mapping: StatusError with the contract's code.
	statusErr := func(name string, err error, status int, code string) {
		t.Helper()
		var se *modelclient.StatusError
		if !errors.As(err, &se) || se.Status != status || se.Code != code {
			t.Errorf("%s: err = %v, want %d %s", name, err, status, code)
		}
	}
	_, _, err = c.Recommend(ctx, modelclient.RecommendRequest{Model: "hybrid", Ratings: ratings, K: 5})
	statusErr("Recommend hybrid", err, 400, "unknown_model")
	_, _, err = c.Recommend(ctx, modelclient.RecommendRequest{Model: "knn", Ratings: ratings, K: 101})
	statusErr("Recommend k=101", err, 400, "bad_request")
	_, _, err = c.Recommend(ctx, modelclient.RecommendRequest{Model: "knn", Ratings: append(ratings, modelclient.RatingIn{MovieID: 1, Rating: 2}), K: 5})
	statusErr("Recommend duplicate movie", err, 400, "bad_request")
	_, _, err = c.Similar(ctx, 999999, "knn", 5)
	statusErr("Similar unknown movie", err, 404, "not_found")
	_, _, err = c.Search(ctx, strings.Repeat("a", 101), 5)
	statusErr("Search 101 chars", err, 400, "bad_request")
	ids := make([]int64, 201)
	for i := range ids {
		ids[i] = int64(i + 1)
	}
	_, _, err = c.MoviesBatch(ctx, ids)
	statusErr("MoviesBatch 201 ids", err, 400, "bad_request")
}

func TestModelRawContract(t *testing.T) {
	client := socketClient(env.modelSock)
	base := "http://recs-model"
	send := func(r request) response { return do(t, client, base, r) }
	expect := func(r request, status int, code string) response {
		t.Helper()
		resp := send(r)
		if resp.Status != status {
			t.Errorf("%s: status %d, want %d: %s", r.name, resp.Status, status, truncate(resp.Body))
		}
		if code != "" && resp.ErrorCode() != code {
			t.Errorf("%s: error code %q, want %q", r.name, resp.ErrorCode(), code)
		}
		if got := resp.Header.Get("X-Model-Version"); got != env.version {
			t.Errorf("%s: X-Model-Version %q, want %q", r.name, got, env.version)
		}
		if id := r.headers["X-Request-ID"]; id != "" && resp.Header.Get("X-Request-ID") != id {
			t.Errorf("%s: X-Request-ID %q, want echo of %q", r.name, resp.Header.Get("X-Request-ID"), id)
		}
		return resp
	}

	tooManyRatings := func() string {
		var b strings.Builder
		b.WriteString(`{"model":"pop","k":5,"ratings":[`)
		for i := 0; i < 5001; i++ {
			if i > 0 {
				b.WriteByte(',')
			}
			fmt.Fprintf(&b, `{"movie_id":%d,"rating":3}`, i+1)
		}
		b.WriteString(`]}`)
		return b.String()
	}()
	huge := `{"model":"pop","k":5,"ratings":[],"pad":"` + strings.Repeat("x", 1<<20) + `"}`

	// recommend
	expect(modelReq("recommend", "recommend knn", "POST", "/v1/recommend", `{"model":"knn","k":5,"ratings":`+someRatings+`}`), 200, "")
	expect(modelReq("recommend", "recommend empty ratings", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[]}`), 200, "")
	r := expect(modelReq("recommend", "recommend unknown ids ignored", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[{"movie_id":777777,"rating":4}],"exclude":[888888]}`), 200, "")
	var rr struct{ Ignored int }
	r.JSON(t, &rr)
	if rr.Ignored != 1 {
		t.Errorf("unknown rating id: ignored = %d, want 1", rr.Ignored)
	}
	expect(modelReq("recommend", "recommend malformed json", "POST", "/v1/recommend", `{"model":`), 400, "invalid_json")
	expect(modelReq("recommend", "recommend unknown field", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[],"extra":1}`), 400, "bad_request")
	expect(modelReq("recommend", "recommend rating 0.3", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[{"movie_id":1,"rating":0.3}]}`), 400, "bad_request")
	expect(modelReq("recommend", "recommend rating 5.5", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[{"movie_id":1,"rating":5.5}]}`), 400, "bad_request")
	expect(modelReq("recommend", "recommend k 0", "POST", "/v1/recommend", `{"model":"pop","k":0,"ratings":[]}`), 400, "bad_request")
	expect(modelReq("recommend", "recommend 5001 ratings", "POST", "/v1/recommend", tooManyRatings), 400, "bad_request")
	expect(modelReq("recommend", "recommend duplicate id", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[{"movie_id":1,"rating":3},{"movie_id":1,"rating":4}]}`), 400, "bad_request")
	expect(modelReq("recommend", "recommend reserved mmr", "POST", "/v1/recommend", `{"model":"mmr","k":5,"ratings":[]}`), 400, "unknown_model")
	expect(modelReq("recommend", "recommend unknown name", "POST", "/v1/recommend", `{"model":"nope","k":5,"ratings":[]}`), 400, "")
	expect(modelReq("recommend", "recommend body over 1 MiB", "POST", "/v1/recommend", huge), 400, "body_too_large")

	// compare
	expect(modelReq("compare", "compare three", "POST", "/v1/compare", `{"models":["pop","knn","mf"],"k":5,"ratings":`+someRatings+`}`), 200, "")
	expect(modelReq("compare", "compare duplicate models", "POST", "/v1/compare", `{"models":["pop","pop"],"k":5,"ratings":[]}`), 400, "bad_request")
	expect(modelReq("compare", "compare six models", "POST", "/v1/compare", `{"models":["pop","pop_count","knn","content","mf","als"],"k":5,"ratings":[]}`), 400, "bad_request")
	expect(modelReq("compare", "compare reserved", "POST", "/v1/compare", `{"models":["pop","hybrid"],"k":5,"ratings":[]}`), 400, "unknown_model")
	expect(modelReq("compare", "compare malformed", "POST", "/v1/compare", `[`), 400, "invalid_json")

	// similar
	expect(modelReq("similar", "similar knn", "GET", "/v1/similar/1?model=knn&k=5", ""), 200, "")
	expect(modelReq("similar", "similar content default k", "GET", "/v1/similar/2?model=content", ""), 200, "")
	expect(modelReq("similar", "similar unknown movie", "GET", "/v1/similar/999999", ""), 404, "not_found")
	expect(modelReq("similar", "similar model als", "GET", "/v1/similar/1?model=als", ""), 400, "")
	expect(modelReq("similar", "similar k 101", "GET", "/v1/similar/1?k=101", ""), 400, "bad_request")
	expect(modelReq("similar", "similar id 0", "GET", "/v1/similar/0", ""), 400, "bad_request")
	expect(modelReq("similar", "similar id not a number", "GET", "/v1/similar/abc", ""), 400, "bad_request")

	// onboarding
	expect(modelReq("onboarding", "onboarding 10", "POST", "/v1/onboarding", `{"n":10,"exclude":[1,2]}`), 200, "")
	expect(modelReq("onboarding", "onboarding n 101", "POST", "/v1/onboarding", `{"n":101}`), 400, "bad_request")
	expect(modelReq("onboarding", "onboarding malformed", "POST", "/v1/onboarding", `{"n":`), 400, "invalid_json")

	// search
	expect(modelReq("search", "search toy", "GET", "/v1/movies/search?q=toy&limit=5", ""), 200, "")
	expect(modelReq("search", "search no match", "GET", "/v1/movies/search?q=zzzzzz", ""), 200, "")
	expect(modelReq("search", "search missing q", "GET", "/v1/movies/search", ""), 400, "bad_request")
	expect(modelReq("search", "search limit 51", "GET", "/v1/movies/search?q=a&limit=51", ""), 400, "bad_request")

	// movies batch
	expect(modelReq("moviesBatch", "batch mixed", "POST", "/v1/movies/batch", `{"ids":[30,1,999999]}`), 200, "")
	expect(modelReq("moviesBatch", "batch empty", "POST", "/v1/movies/batch", `{"ids":[]}`), 400, "bad_request")
	expect(modelReq("moviesBatch", "batch id 0", "POST", "/v1/movies/batch", `{"ids":[0]}`), 400, "bad_request")
	expect(modelReq("moviesBatch", "batch malformed", "POST", "/v1/movies/batch", `{"ids":[1,}`), 400, "invalid_json")

	// version, models, healthz, metrics
	expect(modelReq("version", "version", "GET", "/v1/version", ""), 200, "")
	expect(modelReq("models", "models", "GET", "/v1/models", ""), 200, "")
	expect(modelReq("healthz", "healthz", "GET", "/healthz", ""), 200, "")
	m := expect(modelReq("metrics", "metrics", "GET", "/metrics", ""), 200, "")
	if !strings.Contains(string(m.Body), "# TYPE") {
		t.Errorf("/metrics is not Prometheus text: %s", truncate(m.Body))
	}

	// X-Request-ID: generated when absent or invalid, never echoed when invalid.
	// (recs-model accepts up to 128 characters; recs-api forwards at most 64.)
	for _, id := range []string{"", "has space", "semi;colon", strings.Repeat("a", 129)} {
		req := modelReq("version", "version request id "+fmt.Sprint(len(id)), "GET", "/v1/version", "")
		req.headers = map[string]string{}
		if id != "" {
			req.headers["X-Request-ID"] = id
		}
		resp := send(req)
		got := resp.Header.Get("X-Request-ID")
		if got == "" || got == id || !reqid.Valid(got) {
			t.Errorf("X-Request-ID for sent %q: got %q, want a generated valid ID", id, got)
		}
	}
}

// TestModelUnavailable starts a second recs-model with no artifact: every
// model route is 503 model_unavailable with X-Model-Version: none, and the
// client maps that to ErrUnavailable.
func TestModelUnavailable(t *testing.T) {
	empty := t.TempDir()
	sock := filepath.Join(env.sockDir, "empty.sock")
	cmd, err := startModel(env.modelBin, sock, empty, filepath.Join(env.work, "model-empty.log"))
	if err != nil {
		t.Fatal(err)
	}
	defer stopCmd(cmd)
	if err := waitFor(10*time.Second, func() bool { _, err := os.Stat(sock); return err == nil }); err != nil {
		t.Fatalf("socket never appeared: %v", err)
	}
	if err := waitFor(5*time.Second, func() bool { return socketGet(sock, "/healthz") == 503 }); err != nil {
		t.Fatalf("healthz never answered 503: %v", err)
	}
	client := socketClient(sock)
	cases := []request{
		modelReq("recommend", "unavailable recommend", "POST", "/v1/recommend", `{"model":"pop","k":5,"ratings":[]}`),
		modelReq("compare", "unavailable compare", "POST", "/v1/compare", `{"models":["pop"],"k":5,"ratings":[]}`),
		modelReq("similar", "unavailable similar", "GET", "/v1/similar/1", ""),
		modelReq("onboarding", "unavailable onboarding", "POST", "/v1/onboarding", `{"n":5}`),
		modelReq("search", "unavailable search", "GET", "/v1/movies/search?q=toy", ""),
		modelReq("moviesBatch", "unavailable batch", "POST", "/v1/movies/batch", `{"ids":[1]}`),
		modelReq("version", "unavailable version", "GET", "/v1/version", ""),
		modelReq("models", "unavailable models", "GET", "/v1/models", ""),
		modelReq("healthz", "unavailable healthz", "GET", "/healthz", ""),
	}
	for _, c := range cases {
		resp := do(t, client, "http://recs-model", c)
		if resp.Status != http.StatusServiceUnavailable {
			t.Errorf("%s: status %d, want 503", c.name, resp.Status)
		}
		if c.op != "healthz" && resp.ErrorCode() != "model_unavailable" {
			t.Errorf("%s: code %q, want model_unavailable", c.name, resp.ErrorCode())
		}
		if v := resp.Header.Get("X-Model-Version"); v != "none" {
			t.Errorf("%s: X-Model-Version %q, want none", c.name, v)
		}
	}
	mc := modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: sock, Timeout: time.Second})
	if _, meta, err := mc.Version(context.Background()); !errors.Is(err, modelclient.ErrUnavailable) || meta.ModelVersion != "none" {
		t.Errorf("client Version on empty model: meta %+v err %v, want ErrUnavailable with version none", meta, err)
	}
	if err := mc.Healthz(context.Background()); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Errorf("client Healthz on empty model: %v, want ErrUnavailable", err)
	}
	// And a socket nobody listens on.
	gone := modelclient.NewHTTP(modelclient.HTTPConfig{SocketPath: filepath.Join(env.sockDir, "nobody.sock"), Timeout: time.Second})
	if _, _, err := gone.Version(context.Background()); !errors.Is(err, modelclient.ErrUnavailable) {
		t.Errorf("client with no server: %v, want ErrUnavailable", err)
	}
}
