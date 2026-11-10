// Package stack holds the end-to-end gates that run through Caddy against the
// compose stack (tests/integration/run-stack.sh starts it on 127.0.0.1):
//
//   - N2 robustness: unknown movie ID, empty ratings, out-of-range rating, a
//     65 KB body and malformed JSON each get a clean 4xx with the Error shape.
//   - N3 degradation: with recs-model stopped (and, separately, hung), login,
//     signup and ratings keep working, and the recommendation endpoints answer
//     503 model_unavailable within 1 s.
//
// Every exchange is recorded and checked against contracts/public.yaml after
// the run. Configuration (set by run-stack.sh):
//
//	RECS_STACK_URL           base URL of Caddy, e.g. http://127.0.0.1:18090
//	RECS_STACK_ORIGIN        the site origin recs-api is configured with
//	RECS_STACK_INVITE        a multi-use invite code
//	RECS_STACK_STOP_MODEL    shell command that stops recs-model
//	RECS_STACK_START_MODEL   shell command that starts it again
//	RECS_STACK_PAUSE_MODEL   optional: freezes recs-model (hung, not dead)
//	RECS_STACK_UNPAUSE_MODEL optional: unfreezes it
package stack

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/integrationtest/internal/record"
)

var (
	rec    = &record.Recorder{}
	base   string
	origin string
	host   string
	invite string
)

func TestMain(m *testing.M) {
	base, origin, invite = os.Getenv("RECS_STACK_URL"), os.Getenv("RECS_STACK_ORIGIN"), os.Getenv("RECS_STACK_INVITE")
	if base == "" || origin == "" || invite == "" {
		fmt.Println("stack tests skipped: RECS_STACK_URL, RECS_STACK_ORIGIN and RECS_STACK_INVITE are set by tests/integration/run-stack.sh")
		os.Exit(0)
	}
	u, err := url.Parse(origin)
	if err != nil || u.Host == "" {
		fmt.Fprintln(os.Stderr, "bad RECS_STACK_ORIGIN:", origin)
		os.Exit(2)
	}
	host = u.Host
	code := m.Run()
	repo, err := record.RepoRoot()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	dir, err := os.MkdirTemp("", "intermission-stack-")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if v := rec.Validate(repo, filepath.Join(dir, "observations.jsonl"), false); code == 0 {
		code = v
	}
	_ = os.RemoveAll(dir) // os.Exit skips deferred calls
	os.Exit(code)
}

// client is one browser: its own cookie jar, the site's Host and, on
// mutating requests, the CSRF headers.
type client struct {
	t       *testing.T
	http    *http.Client
	session string // session cookie value
}

func newClient(t *testing.T) *client {
	return &client{t: t, http: &http.Client{Timeout: 10 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}
}

func (c *client) call(op, name, method, path, body string) record.Response {
	c.t.Helper()
	h := map[string]string{"Host": host}
	if method != http.MethodGet {
		h["Origin"] = origin
		h["X-Requested-With"] = "recs"
	}
	// The session cookie is Secure (production is HTTPS through Funnel), so a
	// cookie jar would not send it over this plain-HTTP loopback hop; carry it
	// by hand, as deploy/smoke.sh does.
	if c.session != "" {
		h["Cookie"] = "session=" + c.session
	}
	req := record.Request{Spec: "public", Op: op, Name: name, Method: method, Path: path, Headers: h}
	if body != "" {
		req.Body = []byte(body)
	}
	r := rec.Do(c.t, c.http, base, req)
	for _, line := range r.Header.Values("Set-Cookie") {
		if ck, err := http.ParseSetCookie(line); err == nil && ck.Name == "session" {
			c.session = ck.Value
			if ck.MaxAge < 0 || ck.Value == "" {
				c.session = ""
			}
		}
	}
	return r
}

func (c *client) expect(op, name, method, path, body string, status int, code string) record.Response {
	c.t.Helper()
	r := c.call(op, name, method, path, body)
	if r.Status != status || (code != "" && r.ErrorCode() != code) {
		c.t.Errorf("%s: %d %s, want %d %s: %s", name, r.Status, r.ErrorCode(), status, code, record.Truncate(r.Body))
	}
	return r
}

// expect4xx checks a clean client error: the given status and code, a JSON
// Error body (the schema itself is checked after the run), security headers
// from Caddy, and no hint of a stack trace or upstream failure.
func (c *client) expect4xx(op, name, method, path, body string, status int, code string) {
	c.t.Helper()
	r := c.expect(op, name, method, path, body, status, code)
	if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		c.t.Errorf("%s: Content-Type %q, want application/json", name, ct)
	}
	if r.Header.Get("Content-Security-Policy") == "" {
		c.t.Errorf("%s: no Content-Security-Policy (not served through Caddy?)", name)
	}
	if r.Header.Get("Server") != "" {
		c.t.Errorf("%s: Server header %q exposed", name, r.Header.Get("Server"))
	}
}

func randomName(prefix string) string {
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return prefix + hex.EncodeToString(b)
}

const password = "integration password 1"

// signup creates a fresh account with the shared invite; c holds its session.
func (c *client) signup(name string) {
	c.t.Helper()
	c.expect("signup", "signup "+name, "POST", "/api/auth/signup",
		fmt.Sprintf(`{"username":%q,"password":%q,"invite_code":%q}`, name, password, invite), 201, "")
}

// seenMovie returns the ID of Toy Story via search, which also puts it in
// recs-api's movie cache (needed to rate it while recs-model is down).
func (c *client) seenMovies() []int64 {
	c.t.Helper()
	r := c.expect("searchMovies", "search toy", "GET", "/api/movies/search?q=toy&limit=10", "", 200, "")
	var ml struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
		}
	}
	r.JSON(c.t, &ml)
	r = c.expect("searchMovies", "search star", "GET", "/api/movies/search?q=star&limit=10", "", 200, "")
	var more struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
		}
	}
	r.JSON(c.t, &more)
	ml.Items = append(ml.Items, more.Items...)
	var ids []int64
	for _, it := range ml.Items {
		ids = append(ids, it.MovieID)
	}
	if len(ids) < 6 {
		c.t.Fatalf("search returned %d movies, need 6", len(ids))
	}
	return ids
}

func TestRobustnessThroughCaddy(t *testing.T) {
	c := newClient(t)
	c.signup(randomName("robust-"))
	ids := c.seenMovies()
	rate := fmt.Sprintf("/api/ratings/%d", ids[0])
	big := `{"rating":4,"pad":"` + strings.Repeat("a", 65*1024) + `"}`

	// Unknown movie ID.
	c.expect4xx("getMovie", "unknown movie", "GET", "/api/movies/999999", "", 404, "not_found")
	c.expect4xx("getSimilar", "unknown movie similar", "GET", "/api/movies/999999/similar", "", 404, "not_found")
	c.expect4xx("putRating", "rate unknown movie", "PUT", "/api/ratings/999999", `{"rating":4}`, 404, "not_found")
	c.expect4xx("getMovie", "movie id 0", "GET", "/api/movies/0", "", 400, "bad_request")
	c.expect4xx("getMovie", "movie id negative", "GET", "/api/movies/-5", "", 400, "bad_request")
	c.expect4xx("getMovie", "movie id over int32", "GET", "/api/movies/2147483648", "", 400, "bad_request")
	c.expect4xx("putRating", "rate movie id text", "PUT", "/api/ratings/abc", `{"rating":4}`, 400, "bad_request")

	// Empty ratings: an empty body, an empty object, a null rating.
	c.expect4xx("putRating", "empty body", "PUT", rate, "", 400, "")
	c.expect4xx("putRating", "empty object", "PUT", rate, `{}`, 400, "bad_request")
	c.expect4xx("putRating", "null rating", "PUT", rate, `{"rating":null}`, 400, "bad_request")
	c.expect4xx("putRating", "empty array", "PUT", rate, `[]`, 400, "")
	// A user with no ratings gets popularity, not an error.
	r := c.expect("recommend", "recommend with no ratings", "GET", "/api/recommend", "", 200, "")
	var rr struct {
		ColdStart bool `json:"cold_start"`
	}
	r.JSON(t, &rr)
	if !rr.ColdStart {
		t.Error("no ratings: cold_start false")
	}

	// Out-of-range rating.
	for _, v := range []string{"0", "-1", "0.4", "5.5", "6", "4.3", "1e309", `"5"`, "true"} {
		c.expect4xx("putRating", "rating "+v, "PUT", rate, `{"rating":`+v+`}`, 400, "")
	}

	// A 65 KB body, on every endpoint that takes one.
	c.expect4xx("putRating", "65 KB rating", "PUT", rate, big, 413, "body_too_large")
	c.expect4xx("signup", "65 KB signup", "POST", "/api/auth/signup", big, 413, "body_too_large")
	c.expect4xx("login", "65 KB login", "POST", "/api/auth/login", big, 413, "body_too_large")

	// Malformed JSON.
	for _, b := range []string{`{"rating":`, `{rating: 4}`, `{"rating":4}}`, "\x00\xff"} {
		c.expect4xx("putRating", fmt.Sprintf("malformed rating %q", b), "PUT", rate, b, 400, "")
	}
	c.expect4xx("signup", "malformed signup", "POST", "/api/auth/signup", `{"username":"x",`, 400, "invalid_json")
	c.expect4xx("login", "malformed login", "POST", "/api/auth/login", `{"username"`, 400, "invalid_json")

	// Still fine afterwards.
	c.expect("putRating", "valid rating", "PUT", rate, `{"rating":4.5}`, 200, "")
}

// shell runs one of the configured stack-control commands.
func shell(t *testing.T, envVar string) {
	t.Helper()
	cmdline := os.Getenv(envVar)
	if cmdline == "" {
		t.Fatalf("%s is not set", envVar)
	}
	out, err := exec.Command("bash", "-c", cmdline).CombinedOutput()
	if err != nil {
		t.Fatalf("%s: %v: %s", envVar, err, out)
	}
}

func waitHealth(t *testing.T, c *client, want int, within time.Duration) time.Duration {
	t.Helper()
	start := time.Now()
	for time.Since(start) < within {
		r := c.call("healthz", "healthz wait", "GET", "/healthz", "")
		if r.Status == want {
			return time.Since(start)
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("/healthz never returned %d within %s", want, within)
	return 0
}

func TestDegradationThroughCaddy(t *testing.T) {
	user := randomName("degrade-")
	setup := newClient(t)
	setup.signup(user)
	ids := setup.seenMovies()
	for i, id := range ids[:5] {
		setup.expect("putRating", fmt.Sprintf("setup rating %d", i), "PUT", fmt.Sprintf("/api/ratings/%d", id), `{"rating":4}`, 200, "")
	}
	setup.expect("recommend", "recommend while up", "GET", "/api/recommend?model=knn", "", 200, "")

	modes := []struct{ name, down, up string }{{"stopped", "RECS_STACK_STOP_MODEL", "RECS_STACK_START_MODEL"}}
	if os.Getenv("RECS_STACK_PAUSE_MODEL") != "" {
		modes = append(modes, struct{ name, down, up string }{"hung", "RECS_STACK_PAUSE_MODEL", "RECS_STACK_UNPAUSE_MODEL"})
	}
	for _, mode := range modes {
		t.Run(mode.name, func(t *testing.T) {
			shell(t, mode.down)
			defer func() {
				shell(t, mode.up)
				waitHealth(t, newClient(t), 200, 60*time.Second)
			}()
			anon := newClient(t)
			if mode.name == "stopped" {
				waitHealth(t, anon, 503, 10*time.Second)
			}
			anon.expect("healthz", "healthz degraded "+mode.name, "GET", "/healthz", "", 503, "")

			// Accounts and ratings keep working.
			c := newClient(t)
			c.expect("login", "login "+mode.name, "POST", "/api/auth/login", fmt.Sprintf(`{"username":%q,"password":%q}`, user, password), 200, "")
			c.expect("getMe", "me "+mode.name, "GET", "/api/me", "", 200, "")
			c.expect("putRating", "rate "+mode.name, "PUT", fmt.Sprintf("/api/ratings/%d", ids[5]), `{"rating":3.5}`, 200, "")
			c.expect("putRating", "re-rate "+mode.name, "PUT", fmt.Sprintf("/api/ratings/%d", ids[0]), `{"rating":2}`, 200, "")
			c.expect("listRatings", "ratings "+mode.name, "GET", "/api/ratings", "", 200, "")
			c.expect("deleteRating", "delete rating "+mode.name, "DELETE", fmt.Sprintf("/api/ratings/%d", ids[5]), "", 204, "")
			fresh := newClient(t)
			fresh.signup(randomName("down-"))
			fresh.expect("putRating", "new user rates "+mode.name, "PUT", fmt.Sprintf("/api/ratings/%d", ids[1]), `{"rating":5}`, 200, "")
			fresh.expect("logout", "logout "+mode.name, "POST", "/api/auth/logout", "", 204, "")

			// Recommendation endpoints: 503 model_unavailable within 1 s.
			var worst time.Duration
			for i := 0; i < 3; i++ { // 9 requests: stays under the 30/min recommendation limit
				for _, rq := range []struct{ op, path string }{
					{"recommend", "/api/recommend"},
					{"recommend", "/api/recommend?model=knn&k=10"},
					{"compare", "/api/recommend/compare?models=pop,knn"},
				} {
					r := c.expect(rq.op, fmt.Sprintf("%s %s %d", rq.path, mode.name, i), "GET", rq.path, "", 503, "model_unavailable")
					if r.Elapsed > worst {
						worst = r.Elapsed
					}
					if r.Elapsed >= time.Second {
						t.Errorf("%s (%s): %s, want < 1 s", rq.path, mode.name, r.Elapsed)
					}
				}
			}
			t.Logf("recs-model %s: slowest model_unavailable answer %s (9 requests)", mode.name, worst.Round(time.Millisecond))
		})
	}
	setup.expect("recommend", "recommend after recovery", "GET", "/api/recommend?model=knn", "", 200, "")
}
