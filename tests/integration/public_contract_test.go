package integrationtest

// Gate N1, public side: recs-api's public endpoints (contracts/public.yaml)
// with the real recs-model behind it, not the fake. Every exchange is
// recorded and validated against the contract after the run; the test itself
// checks the behaviour the schema can't express (which code, which status).
//
// One test function with ordered steps: later steps depend on the account and
// ratings created earlier, and the last step stops recs-model.

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/cookiejar"
	"strings"
	"testing"
	"time"
)

type publicClient struct {
	t      *testing.T
	http   *http.Client
	base   string
	origin string
}

func newPublicClient(t *testing.T) *publicClient {
	jar, _ := cookiejar.New(nil)
	return &publicClient{t: t, base: "http://" + env.apiAddr, origin: env.origin,
		http: &http.Client{Jar: jar, Timeout: 10 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}
}

// call sends one request. Mutating methods get the CSRF headers unless
// noCSRF is set.
func (c *publicClient) call(op, name, method, path string, body any, noCSRF bool) response {
	c.t.Helper()
	req := request{spec: "public", op: op, name: name, method: method, path: path, headers: map[string]string{}}
	switch b := body.(type) {
	case nil:
	case []byte:
		req.body = b
	case string:
		req.body = []byte(b)
	default:
		data, err := json.Marshal(b)
		if err != nil {
			c.t.Fatal(err)
		}
		req.body = data
	}
	if method != "GET" && !noCSRF {
		req.headers["Origin"] = c.origin
		req.headers["X-Requested-With"] = "recs"
	}
	return do(c.t, c.http, c.base, req)
}

func (c *publicClient) expect(op, name, method, path string, body any, status int, code string) response {
	c.t.Helper()
	resp := c.call(op, name, method, path, body, false)
	checkResp(c.t, name, resp, status, code)
	return resp
}

func checkResp(t *testing.T, name string, resp response, status int, code string) {
	t.Helper()
	if resp.Status != status {
		t.Errorf("%s: status %d, want %d: %s", name, resp.Status, status, truncate(resp.Body))
	}
	if code != "" && resp.ErrorCode() != code {
		t.Errorf("%s: error code %q, want %q", name, resp.ErrorCode(), code)
	}
	if resp.Header.Get("X-Request-ID") == "" {
		t.Errorf("%s: no X-Request-ID", name)
	}
}

func creds(user, pass string) map[string]string {
	return map[string]string{"username": user, "password": pass}
}

func TestPublicContract(t *testing.T) {
	needStack(t)
	anon := newPublicClient(t)
	alice := newPublicClient(t)
	const pass = "correct horse battery"

	// ---- health, catalog and reports (no session) ----
	anon.expect("healthz", "healthz ok", "GET", "/healthz", nil, 200, "")
	r := anon.expect("searchMovies", "search toy", "GET", "/api/movies/search?q=toy&limit=5", nil, 200, "")
	var ml struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
			Title   string
		}
	}
	r.JSON(t, &ml)
	if len(ml.Items) == 0 || ml.Items[0].MovieID != 1 {
		t.Errorf("search toy: %+v, want Toy Story (1) first", ml.Items)
	}
	anon.expect("searchMovies", "search missing q", "GET", "/api/movies/search", nil, 400, "bad_request")
	anon.expect("searchMovies", "search limit 51", "GET", "/api/movies/search?q=a&limit=51", nil, 400, "bad_request")
	anon.expect("getMovie", "movie 1", "GET", "/api/movies/1", nil, 200, "")
	anon.expect("getMovie", "movie never rated", "GET", "/api/movies/30", nil, 200, "")
	anon.expect("getMovie", "movie unknown", "GET", "/api/movies/999999", nil, 404, "not_found")
	anon.expect("getMovie", "movie id 0", "GET", "/api/movies/0", nil, 400, "bad_request")
	anon.expect("getMovie", "movie id text", "GET", "/api/movies/abc", nil, 400, "bad_request")
	anon.expect("getSimilar", "similar knn", "GET", "/api/movies/1/similar?k=5", nil, 200, "")
	anon.expect("getSimilar", "similar content", "GET", "/api/movies/2/similar?model=content", nil, 200, "")
	anon.expect("getSimilar", "similar unknown movie", "GET", "/api/movies/999999/similar", nil, 404, "not_found")
	anon.expect("getSimilar", "similar k 0", "GET", "/api/movies/1/similar?k=0", nil, 400, "bad_request")
	anon.expect("getSimilar", "similar model als", "GET", "/api/movies/1/similar?model=als", nil, 400, "")
	r = anon.expect("listModels", "models", "GET", "/api/models", nil, 200, "")
	var models struct {
		ArtifactVersion string `json:"artifact_version"`
		DefaultModel    string `json:"default_model"`
	}
	r.JSON(t, &models)
	if models.ArtifactVersion != env.version || models.DefaultModel != "knn" {
		t.Errorf("models: %+v", models)
	}
	r = anon.expect("listReports", "reports", "GET", "/api/reports", nil, 200, "")
	var reports struct {
		Items []struct {
			RunID         string `json:"run_id"`
			SchemaVersion int    `json:"schema_version"`
		}
	}
	r.JSON(t, &reports)
	if len(reports.Items) < 2 {
		t.Fatalf("reports: %d items, want the fixture reports", len(reports.Items))
	}
	for _, it := range reports.Items {
		anon.expect("getReport", "report "+it.RunID, "GET", "/api/reports/"+it.RunID, nil, 200, "")
	}
	anon.expect("getReport", "report unknown", "GET", "/api/reports/no-such-run", nil, 404, "not_found")
	anon.expect("getReport", "report bad id", "GET", "/api/reports/bad.id", nil, 400, "bad_request")
	r = anon.expect("getPoster", "poster placeholder", "GET", "/api/posters/1", nil, 200, "")
	if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "image/") {
		t.Errorf("poster content type %q", ct)
	}
	anon.expect("getPoster", "poster bad id", "GET", "/api/posters/abc", nil, 400, "bad_request")
	// No TMDB key in the stack: details are the empty answer, still 200.
	r = anon.expect("getMovieDetails", "details without TMDB", "GET", "/api/movies/1/details", nil, 200, "")
	var details struct {
		MovieID  int64   `json:"movieId"`
		Overview *string `json:"overview"`
		Cast     []any   `json:"cast"`
	}
	r.JSON(t, &details)
	if details.MovieID != 1 || details.Overview != nil || details.Cast == nil || len(details.Cast) != 0 {
		t.Errorf("details without TMDB: %+v", details)
	}
	anon.expect("getMovieDetails", "details bad id", "GET", "/api/movies/abc/details", nil, 400, "bad_request")

	// ---- session-only routes without a session ----
	anon.expect("getMe", "me no session", "GET", "/api/me", nil, 401, "unauthorized")
	anon.expect("deleteMe", "delete me no session", "DELETE", "/api/me", nil, 401, "unauthorized")
	anon.expect("getOnboarding", "onboarding no session", "GET", "/api/onboarding", nil, 401, "unauthorized")
	anon.expect("listRatings", "ratings no session", "GET", "/api/ratings", nil, 401, "unauthorized")
	anon.expect("putRating", "put rating no session", "PUT", "/api/ratings/1", `{"rating":4}`, 401, "unauthorized")
	anon.expect("deleteRating", "delete rating no session", "DELETE", "/api/ratings/1", nil, 401, "unauthorized")
	anon.expect("listNotInterested", "not interested list no session", "GET", "/api/feedback/not-interested", nil, 401, "unauthorized")
	anon.expect("deleteNotInterested", "undo not interested no session", "DELETE", "/api/feedback/not-interested/1", nil, 401, "unauthorized")
	anon.expect("recommend", "recommend no session", "GET", "/api/recommend", nil, 401, "unauthorized")
	anon.expect("compare", "compare no session", "GET", "/api/recommend/compare?models=pop,knn", nil, 401, "unauthorized")
	anon.expect("logout", "logout no session", "POST", "/api/auth/logout", nil, 401, "unauthorized")

	// ---- signup ----
	big := []byte(`{"username":"x","password":"` + strings.Repeat("a", 70<<10) + `"}`)
	anon.expect("signup", "signup without invite", "POST", "/api/auth/signup", creds("alice", pass), 400, "invalid_invite")
	anon.expect("signup", "signup malformed", "POST", "/api/auth/signup", `{"username":`, 400, "invalid_json")
	anon.expect("signup", "signup short password", "POST", "/api/auth/signup", map[string]string{"username": "alice", "password": "short", "invite_code": "x"}, 400, "bad_request")
	anon.expect("signup", "signup 70 KB body", "POST", "/api/auth/signup", big, 413, "body_too_large")
	checkResp(t, "signup no csrf", anon.call("signup", "signup no csrf", "POST", "/api/auth/signup", creds("alice", pass), true), 403, "csrf_rejected")
	code, err := env.invite(3)
	if err != nil {
		t.Fatal(err)
	}
	signup := map[string]string{"username": "alice", "password": pass, "invite_code": code}
	r = alice.expect("signup", "signup alice", "POST", "/api/auth/signup", signup, 201, "")
	if !strings.Contains(r.Header.Get("Set-Cookie"), "session=") || !strings.Contains(r.Header.Get("Set-Cookie"), "HttpOnly") {
		t.Errorf("signup Set-Cookie: %q", r.Header.Get("Set-Cookie"))
	}
	anon.expect("signup", "signup taken", "POST", "/api/auth/signup", signup, 409, "username_taken")

	// ---- login / me ----
	anon.expect("login", "login wrong password", "POST", "/api/auth/login", creds("alice", "wrong password!"), 401, "invalid_credentials")
	anon.expect("login", "login unknown user", "POST", "/api/auth/login", creds("nobody-here", pass), 401, "invalid_credentials")
	anon.expect("login", "login malformed", "POST", "/api/auth/login", `{`, 400, "invalid_json")
	anon.expect("login", "login 70 KB body", "POST", "/api/auth/login", big, 413, "body_too_large")
	checkResp(t, "login no csrf", anon.call("login", "login no csrf", "POST", "/api/auth/login", creds("alice", pass), true), 403, "csrf_rejected")
	alice.expect("login", "login alice", "POST", "/api/auth/login", creds("alice", pass), 200, "")
	r = alice.expect("getMe", "me", "GET", "/api/me", nil, 200, "")
	var me struct {
		Username    string
		RatingCount int `json:"rating_count"`
	}
	r.JSON(t, &me)
	if me.Username != "alice" || me.RatingCount != 0 {
		t.Errorf("me: %+v", me)
	}

	// ---- onboarding, ratings ----
	r = alice.expect("getOnboarding", "onboarding", "GET", "/api/onboarding?n=10", nil, 200, "")
	alice.expect("getOnboarding", "onboarding n 61", "GET", "/api/onboarding?n=61", nil, 400, "bad_request")
	// Cold start first: fewer than 5 ratings.
	alice.expect("putRating", "rate 1", "PUT", "/api/ratings/1", `{"rating":5}`, 200, "")
	r = alice.expect("recommend", "recommend cold start", "GET", "/api/recommend?model=knn&k=5", nil, 200, "")
	var rec struct {
		Model     string
		ColdStart bool `json:"cold_start"`
		Items     []struct {
			MovieID int64 `json:"movieId"`
		}
	}
	r.JSON(t, &rec)
	if !rec.ColdStart || rec.Model != "pop" {
		t.Errorf("1 rating: cold_start %v model %s, want true/pop", rec.ColdStart, rec.Model)
	}
	for id, v := range map[int]string{2: "4.5", 3: "4", 7: "3.5", 11: "5", 20: "0.5"} {
		alice.expect("putRating", fmt.Sprintf("rate %d", id), "PUT", fmt.Sprintf("/api/ratings/%d", id), `{"rating":`+v+`}`, 200, "")
	}
	alice.expect("putRating", "rate update", "PUT", "/api/ratings/20", `{"rating":1}`, 200, "")
	alice.expect("putRating", "rating 7", "PUT", "/api/ratings/3", `{"rating":7}`, 400, "bad_request")
	alice.expect("putRating", "rating 0", "PUT", "/api/ratings/3", `{"rating":0}`, 400, "bad_request")
	alice.expect("putRating", "rating 4.3", "PUT", "/api/ratings/3", `{"rating":4.3}`, 400, "bad_request")
	alice.expect("putRating", "rating empty body", "PUT", "/api/ratings/3", `{}`, 400, "bad_request")
	alice.expect("putRating", "rating malformed", "PUT", "/api/ratings/3", `{"rating":`, 400, "invalid_json")
	alice.expect("putRating", "rating unknown movie", "PUT", "/api/ratings/999999", `{"rating":4}`, 404, "not_found")
	alice.expect("putRating", "rating 70 KB body", "PUT", "/api/ratings/3", big, 413, "body_too_large")
	checkResp(t, "rating no csrf", alice.call("putRating", "rating no csrf", "PUT", "/api/ratings/3", `{"rating":4}`, true), 403, "csrf_rejected")
	alice.expect("deleteRating", "delete rating", "DELETE", "/api/ratings/20", nil, 204, "")
	alice.expect("deleteRating", "delete rating not rated", "DELETE", "/api/ratings/29", nil, 204, "")
	alice.expect("deleteRating", "delete rating bad id", "DELETE", "/api/ratings/abc", nil, 400, "bad_request")
	checkResp(t, "delete rating no csrf", alice.call("deleteRating", "delete rating no csrf", "DELETE", "/api/ratings/2", nil, true), 403, "csrf_rejected")

	// ---- not interested: mark, list, undo ----
	alice.expect("postFeedback", "not interested 5", "POST", "/api/feedback", `{"movieId":5,"model":"knn","action":"not_interested"}`, 204, "")
	r = alice.expect("listNotInterested", "not interested list", "GET", "/api/feedback/not-interested", nil, 200, "")
	var hidden struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
			Title   string
		}
	}
	r.JSON(t, &hidden)
	if len(hidden.Items) != 1 || hidden.Items[0].MovieID != 5 || hidden.Items[0].Title == "" {
		t.Errorf("not interested list: %+v", hidden)
	}
	alice.expect("deleteNotInterested", "undo not interested", "DELETE", "/api/feedback/not-interested/5", nil, 204, "")
	alice.expect("deleteNotInterested", "undo not interested again", "DELETE", "/api/feedback/not-interested/5", nil, 204, "")
	alice.expect("deleteNotInterested", "undo not interested bad id", "DELETE", "/api/feedback/not-interested/abc", nil, 400, "bad_request")
	checkResp(t, "undo not interested no csrf", alice.call("deleteNotInterested", "undo not interested no csrf", "DELETE", "/api/feedback/not-interested/5", nil, true), 403, "csrf_rejected")
	r = alice.expect("listRatings", "ratings", "GET", "/api/ratings", nil, 200, "")
	var rl struct{ Items []json.RawMessage }
	r.JSON(t, &rl)
	if len(rl.Items) != 5 {
		t.Errorf("ratings: %d items, want 5", len(rl.Items))
	}

	// ---- recommendations ----
	r = alice.expect("recommend", "recommend default", "GET", "/api/recommend", nil, 200, "")
	r.JSON(t, &rec)
	if rec.ColdStart || rec.Model != "knn" || len(rec.Items) == 0 {
		t.Errorf("5 ratings, default model: %+v", rec)
	}
	if r.Header.Get("X-Model-Version") != env.version {
		t.Errorf("recommend X-Model-Version %q", r.Header.Get("X-Model-Version"))
	}
	for _, m := range []string{"pop", "pop_count", "knn", "content", "mf", "als"} {
		alice.expect("recommend", "recommend "+m, "GET", "/api/recommend?model="+m+"&k=5", nil, 200, "")
	}
	alice.expect("recommend", "recommend hybrid", "GET", "/api/recommend?model=hybrid", nil, 400, "unknown_model")
	alice.expect("recommend", "recommend bogus model", "GET", "/api/recommend?model=bogus", nil, 400, "unknown_model")
	alice.expect("recommend", "recommend k 101", "GET", "/api/recommend?k=101", nil, 400, "bad_request")
	alice.expect("compare", "compare", "GET", "/api/recommend/compare?models=pop,knn,als&k=5", nil, 200, "")
	alice.expect("compare", "compare duplicate", "GET", "/api/recommend/compare?models=pop,pop", nil, 400, "bad_request")
	alice.expect("compare", "compare missing", "GET", "/api/recommend/compare", nil, 400, "bad_request")
	alice.expect("getMovie", "movie with user rating", "GET", "/api/movies/1", nil, 200, "")

	// Rate limit: 30 recommendation calls per minute per session and user.
	// Calls so far this minute: 11. Keep going until 429.
	var limited response
	for i := 0; i < 40; i++ {
		resp := alice.call("compare", fmt.Sprintf("compare until limited %d", i), "GET", "/api/recommend/compare?models=pop,knn&k=3", nil, false)
		if resp.Status == http.StatusTooManyRequests {
			limited = resp
			break
		}
	}
	checkResp(t, "compare rate limited", limited, 429, "rate_limited")
	if limited.Header.Get("Retry-After") == "" {
		t.Error("429 without Retry-After")
	}
	checkResp(t, "recommend rate limited", alice.call("recommend", "recommend rate limited", "GET", "/api/recommend", nil, false), 429, "rate_limited")

	// Login limit: 5 per minute per account name, whether or not it exists.
	var loginLimited response
	for i := 0; i < 8; i++ {
		resp := anon.call("login", fmt.Sprintf("login until limited %d", i), "POST", "/api/auth/login", creds("ghost-user", "wrong password!"), false)
		if resp.Status == http.StatusTooManyRequests {
			loginLimited = resp
			break
		}
	}
	checkResp(t, "login rate limited", loginLimited, 429, "rate_limited")

	// ---- the 5,000-rating cap ----
	// The tiny catalog has 30 movies, so alice's other 4,995 ratings are written straight into
	// the scratch app.db (ratings.movie_id has no foreign key; recs-api checks the catalog only
	// when a rating is written through it). Then the real recs-api must refuse a 5,001st movie
	// and still accept re-rating one she has.
	if err := env.seedRatings("alice", 5000); err != nil {
		t.Fatalf("seed ratings: %v", err)
	}
	alice.expect("putRating", "5,001st rating", "PUT", "/api/ratings/6", `{"rating":4}`, 409, "too_many_ratings")
	alice.expect("putRating", "re-rate at the cap", "PUT", "/api/ratings/1", `{"rating":3.5}`, 200, "")
	r = alice.expect("getMe", "me at the cap", "GET", "/api/me", nil, 200, "")
	r.JSON(t, &me)
	if me.RatingCount != 5000 {
		t.Errorf("rating_count at the cap = %d, want 5000", me.RatingCount)
	}

	// ---- logout and delete ----
	bob := newPublicClient(t)
	bob.expect("signup", "signup bob", "POST", "/api/auth/signup", map[string]string{"username": "bob", "password": pass, "invite_code": code}, 201, "")
	// DELETE limits: 60 per minute per user on each of the two DELETE routes,
	// separate budgets. bob's buckets are fresh, so his first 60 calls to each
	// route succeed (refills only add tokens) and a 429 follows. Up to 120
	// calls leave room for refills on a slow runner (one token per second).
	for _, d := range []struct{ op, path string }{
		{"deleteRating", "/api/ratings/1"},
		{"deleteNotInterested", "/api/feedback/not-interested/1"},
	} {
		var deleteLimited response
		for i := 0; i < 120; i++ {
			resp := bob.call(d.op, fmt.Sprintf("%s until limited %d", d.op, i), "DELETE", d.path, nil, false)
			if resp.Status == http.StatusTooManyRequests {
				if i < 60 {
					t.Errorf("%s: 429 on call %d, want the first 60 to succeed", d.op, i+1)
				}
				deleteLimited = resp
				break
			}
			if resp.Status != http.StatusNoContent {
				t.Errorf("%s call %d: status %d, want 204", d.op, i+1, resp.Status)
			}
		}
		checkResp(t, d.op+" rate limited", deleteLimited, 429, "rate_limited")
		if deleteLimited.Header.Get("Retry-After") == "" {
			t.Errorf("%s: 429 without Retry-After", d.op)
		}
	}
	bob.expect("logout", "logout bob", "POST", "/api/auth/logout", nil, 204, "")
	bob.expect("getMe", "me after logout", "GET", "/api/me", nil, 401, "unauthorized")
	checkResp(t, "logout no csrf", alice.call("logout", "logout no csrf", "POST", "/api/auth/logout", nil, true), 403, "csrf_rejected")
	checkResp(t, "delete me no csrf", alice.call("deleteMe", "delete me no csrf", "DELETE", "/api/me", nil, true), 403, "csrf_rejected")
	carol := newPublicClient(t)
	carol.expect("signup", "signup carol", "POST", "/api/auth/signup", map[string]string{"username": "carol", "password": pass, "invite_code": code}, 201, "")
	carol.expect("putRating", "carol rates", "PUT", "/api/ratings/5", `{"rating":3}`, 200, "")
	carol.expect("deleteMe", "delete carol", "DELETE", "/api/me", nil, 204, "")
	carol.expect("getMe", "me after delete", "GET", "/api/me", nil, 401, "unauthorized")
	anon.expect("login", "login deleted account", "POST", "/api/auth/login", creds("carol", pass), 401, "invalid_credentials")
	// Invite used up (3 uses: alice, bob, carol).
	anon.expect("signup", "signup invite used up", "POST", "/api/auth/signup", map[string]string{"username": "dave", "password": pass, "invite_code": code}, 400, "invalid_invite")
	// Signup limit: 10 per minute across all callers. 6 attempts that reached
	// the limiter so far; keep going until 429.
	var signupLimited response
	for i := 0; i < 12; i++ {
		resp := anon.call("signup", fmt.Sprintf("signup until limited %d", i), "POST", "/api/auth/signup", map[string]string{"username": "eve", "password": pass, "invite_code": "nope"}, false)
		if resp.Status == http.StatusTooManyRequests {
			signupLimited = resp
			break
		}
	}
	checkResp(t, "signup rate limited", signupLimited, 429, "rate_limited")

	// ---- recs-model stopped: model routes 503, the rest keep working ----
	dave := newPublicClient(t)
	// A session for after the stop (login doesn't need recs-model). bob, since
	// alice's recommendation budget is spent.
	dave.expect("login", "login bob again", "POST", "/api/auth/login", creds("bob", pass), 200, "")
	env.stopModel()
	anon.expect("healthz", "healthz degraded", "GET", "/healthz", nil, 503, "")
	unavailable := []struct{ op, name, path string }{
		{"searchMovies", "search model down", "/api/movies/search?q=heat"},
		// recs-api has never seen 999998, so it cannot tell whether it exists.
		{"getMovie", "movie never seen, model down", "/api/movies/999998"},
		{"getSimilar", "similar model down", "/api/movies/1/similar"},
		{"listModels", "models model down", "/api/models"},
	}
	for _, u := range unavailable {
		resp := anon.expect(u.op, u.name, "GET", u.path, nil, 503, "model_unavailable")
		if resp.Elapsed > time.Second {
			t.Errorf("%s took %s, want < 1 s", u.name, resp.Elapsed)
		}
	}
	for _, u := range []struct{ op, name, path string }{
		{"getOnboarding", "onboarding model down", "/api/onboarding"},
		{"recommend", "recommend model down", "/api/recommend?model=knn"},
		{"compare", "compare model down", "/api/recommend/compare?models=pop,knn"},
	} {
		resp := dave.expect(u.op, u.name, "GET", u.path, nil, 503, "model_unavailable")
		if resp.Elapsed > time.Second {
			t.Errorf("%s took %s, want < 1 s", u.name, resp.Elapsed)
		}
	}
	dave.expect("putRating", "rate seen movie, model down", "PUT", "/api/ratings/2", `{"rating":3}`, 200, "")
	dave.expect("putRating", "rate never-seen movie, model down", "PUT", "/api/ratings/999997", `{"rating":3}`, 503, "model_unavailable")
	dave.expect("listRatings", "ratings model down", "GET", "/api/ratings", nil, 200, "")
	dave.expect("getMovie", "seen movie, model down", "GET", "/api/movies/1", nil, 200, "")
	anon.expect("listReports", "reports model down", "GET", "/api/reports", nil, 200, "")
}
