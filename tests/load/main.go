// Command recs-load measures /api/recommend latency through Caddy (spec, Phase 2: "KNN, warm
// cache miss: p99 under 75 ms at 20 concurrent clients on the host machine").
//
// Setup (not timed): sign up -users accounts with one invite code (signup is limited to 10 per
// minute, so this takes a few minutes), and give each one -ratings ratings on popular movies
// from /api/onboarding.
//
// Measurement: -clients workers run closed-loop with no think time. Each iteration takes a user
// that still has recommendation budget, changes one of that user's ratings (PUT, not timed;
// it bumps the user's ratings_version, so the recommendation cache misses), then times
// GET /api/recommend?model=<model>&k=<k> until the whole body is read. Every response must be
// 200 with cold_start false, the requested model, at least one item and an X-Model-Version
// header; anything else is an error and fails the run.
//
// recs-api allows 30 recommendation calls per minute per user (burst 30), so each user is
// used at most 30 times; the timed sample is users × 30 − warmup requests. Warm: the first
// -warmup timed requests (and the setup's onboarding calls) warm recs-model and recs-api's
// movie cache and are discarded.
//
// The session cookie is Secure, so it is carried by hand over this plain-HTTP loopback hop.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"math/rand/v2"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type config struct {
	base, origin, invite, model, label, out string
	users, clients, ratings, k, warmup      int
	perUser                                 int
	seed                                    uint64
	thresholdMS                             float64
	beforeMeasure                           string
}

func main() {
	var c config
	flag.StringVar(&c.base, "base", "http://127.0.0.1:18090", "Caddy base URL")
	flag.StringVar(&c.origin, "origin", "http://intermission.test", "site origin recs-api is configured with (sets Host and Origin)")
	flag.StringVar(&c.invite, "invite", "", "invite code with at least -users uses (required)")
	flag.StringVar(&c.model, "model", "knn", "model to request")
	flag.StringVar(&c.label, "label", "", "free-text label for the report (dataset, config)")
	flag.StringVar(&c.out, "out", "", "write the JSON summary here too")
	flag.IntVar(&c.users, "users", 70, "accounts to create")
	flag.IntVar(&c.clients, "clients", 20, "concurrent clients")
	flag.IntVar(&c.ratings, "ratings", 20, "ratings per account")
	flag.IntVar(&c.k, "k", 20, "k for /api/recommend")
	flag.IntVar(&c.warmup, "warmup", 100, "timed requests to discard first")
	flag.IntVar(&c.perUser, "per-user", 30, "recommendation calls per user (recs-api's per-user burst)")
	flag.Uint64Var(&c.seed, "seed", 42, "seed for ratings and movie choice")
	flag.Float64Var(&c.thresholdMS, "p99-ms", 75, "fail if p99 is at or above this many milliseconds")
	flag.StringVar(&c.beforeMeasure, "before-measure", "", "shell command run (and waited for) between setup and measurement, e.g. waiting for a quiet host")
	flag.Parse()
	if c.invite == "" {
		fmt.Fprintln(os.Stderr, "-invite is required")
		os.Exit(2)
	}
	if err := run(c); err != nil {
		fmt.Fprintln(os.Stderr, "FAIL:", err)
		os.Exit(1)
	}
}

type site struct {
	base, origin, host string
	http               *http.Client
}

type user struct {
	name, session string
	movies        []int64
	uses          int
	mu            sync.Mutex // one request at a time per user
}

func (s *site) do(method, path, session string, body any) (*http.Response, []byte, error) {
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return nil, nil, err
		}
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, s.base+path, rd)
	if err != nil {
		return nil, nil, err
	}
	req.Host = s.host
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if method != http.MethodGet {
		req.Header.Set("Origin", s.origin)
		req.Header.Set("X-Requested-With", "recs")
	}
	if session != "" {
		req.Header.Set("Cookie", "session="+session)
	}
	resp, err := s.http.Do(req)
	if err != nil {
		return nil, nil, err
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	return resp, data, err
}

func sessionOf(resp *http.Response) string {
	for _, line := range resp.Header.Values("Set-Cookie") {
		if ck, err := http.ParseSetCookie(line); err == nil && ck.Name == "session" {
			return ck.Value
		}
	}
	return ""
}

// signup creates one account, waiting out the global signup rate limit.
func (s *site) signup(name, invite string) (string, error) {
	body := map[string]string{"username": name, "password": "load test password " + name, "invite_code": invite}
	for attempt := 0; attempt < 30; attempt++ {
		resp, data, err := s.do("POST", "/api/auth/signup", "", body)
		if err != nil {
			return "", err
		}
		switch resp.StatusCode {
		case http.StatusCreated:
			return sessionOf(resp), nil
		case http.StatusTooManyRequests:
			time.Sleep(retryAfter(resp))
			continue
		default:
			return "", fmt.Errorf("signup %s: %d %s", name, resp.StatusCode, data)
		}
	}
	return "", fmt.Errorf("signup %s: still rate limited", name)
}

func retryAfter(resp *http.Response) time.Duration {
	var n int
	if _, err := fmt.Sscan(resp.Header.Get("Retry-After"), &n); err == nil && n > 0 {
		return time.Duration(n) * time.Second
	}
	return 2 * time.Second
}

var halfSteps = []float64{0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5}

func (s *site) setup(c config, rng *rand.Rand) ([]*user, error) {
	stamp := time.Now().UTC().Format("150405")
	var users []*user
	for i := 0; i < c.users; i++ {
		name := fmt.Sprintf("load-%s-%03d", stamp, i)
		session, err := s.signup(name, c.invite)
		if err != nil {
			return nil, err
		}
		if session == "" {
			return nil, errors.New("signup set no session cookie")
		}
		u := &user{name: name, session: session}
		resp, data, err := s.do("GET", "/api/onboarding?n=60", session, nil)
		if err != nil || resp.StatusCode != 200 {
			return nil, fmt.Errorf("onboarding: %v %s", err, data)
		}
		var ob struct {
			Items []struct {
				MovieID int64 `json:"movieId"`
			}
		}
		if err := json.Unmarshal(data, &ob); err != nil {
			return nil, err
		}
		rng.Shuffle(len(ob.Items), func(a, b int) { ob.Items[a], ob.Items[b] = ob.Items[b], ob.Items[a] })
		for _, it := range ob.Items[:min(c.ratings, len(ob.Items))] {
			v := halfSteps[2+rng.IntN(8)] // 1.5 .. 5
			if resp, data, err := s.do("PUT", fmt.Sprintf("/api/ratings/%d", it.MovieID), session, map[string]float64{"rating": v}); err != nil || resp.StatusCode != 200 {
				return nil, fmt.Errorf("rating: %v %s", err, data)
			}
			u.movies = append(u.movies, it.MovieID)
		}
		if len(u.movies) < 5 {
			return nil, fmt.Errorf("%s has only %d ratings (onboarding too small?)", name, len(u.movies))
		}
		users = append(users, u)
		if (i+1)%10 == 0 {
			fmt.Fprintf(os.Stderr, "setup: %d/%d users\n", i+1, c.users)
		}
	}
	return users, nil
}

type sample struct {
	elapsed time.Duration
	err     error
}

func run(c config) error {
	u, err := url.Parse(c.origin)
	if err != nil || u.Host == "" {
		return fmt.Errorf("bad -origin %q", c.origin)
	}
	s := &site{base: strings.TrimRight(c.base, "/"), origin: c.origin, host: u.Host, http: &http.Client{
		Timeout:   10 * time.Second,
		Transport: &http.Transport{MaxIdleConns: 256, MaxIdleConnsPerHost: 256, IdleConnTimeout: 90 * time.Second},
	}}
	rng := rand.New(rand.NewPCG(c.seed, c.seed^0x9e3779b97f4a7c15))
	setupStart := time.Now()
	users, err := s.setup(c, rng)
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "setup done in %s: %d users with %d ratings each\n", time.Since(setupStart).Round(time.Second), len(users), c.ratings)

	if c.beforeMeasure != "" {
		cmd := exec.Command("bash", "-c", c.beforeMeasure)
		cmd.Stdout, cmd.Stderr = os.Stderr, os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("-before-measure: %w", err)
		}
	}

	total := len(users) * c.perUser
	if total <= c.warmup {
		return fmt.Errorf("users*per-user (%d) must exceed warmup (%d)", total, c.warmup)
	}
	var next atomic.Int64 // ticket: which (user, use) to run next
	samples := make([]sample, total)
	load0 := loadavg()
	start := time.Now()
	var wg sync.WaitGroup
	for w := 0; w < c.clients; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			wr := rand.New(rand.NewPCG(c.seed+uint64(w)+1, 7))
			for {
				i := int(next.Add(1) - 1)
				if i >= total {
					return
				}
				// Round-robin over users, so no user is hammered back to back.
				usr := users[i%len(users)]
				samples[i] = once(s, c, usr, wr)
			}
		}(w)
	}
	wg.Wait()
	wall := time.Since(start)
	load1 := loadavg()

	timed := samples[c.warmup:]
	var lat []float64
	var errs []string
	for _, sm := range timed {
		if sm.err != nil {
			errs = append(errs, sm.err.Error())
			continue
		}
		lat = append(lat, float64(sm.elapsed.Microseconds())/1000)
	}
	slices.Sort(lat)
	q := func(p float64) float64 {
		if len(lat) == 0 {
			return math.NaN()
		}
		return lat[min(len(lat)-1, int(math.Ceil(p*float64(len(lat))))-1)]
	}
	sum := 0.0
	for _, v := range lat {
		sum += v
	}
	summary := map[string]any{
		"label": c.label, "model": c.model, "k": c.k, "clients": c.clients, "users": len(users),
		"ratings_per_user": c.ratings, "requests_total": total, "warmup_discarded": c.warmup,
		"timed_requests": len(timed), "errors": len(errs), "wall_seconds": wall.Seconds(),
		"throughput_rps":   float64(total) / wall.Seconds(),
		"latency_ms":       map[string]float64{"p50": q(0.50), "p90": q(0.90), "p95": q(0.95), "p99": q(0.99), "max": q(1), "mean": sum / float64(max(1, len(lat)))},
		"p99_threshold_ms": c.thresholdMS, "loadavg_before": load0, "loadavg_after": load1,
		"measured_at": time.Now().UTC().Format(time.RFC3339),
	}
	out, _ := json.MarshalIndent(summary, "", "  ")
	fmt.Println(string(out))
	if c.out != "" {
		_ = os.WriteFile(c.out, append(out, '\n'), 0o644)
	}
	if len(errs) > 0 {
		show := errs[:min(5, len(errs))]
		return fmt.Errorf("%d of %d timed requests failed, e.g. %s", len(errs), len(timed), strings.Join(show, " | "))
	}
	if p99 := q(0.99); !(p99 < c.thresholdMS) {
		return fmt.Errorf("p99 %.1f ms is not under %.0f ms", p99, c.thresholdMS)
	}
	fmt.Fprintf(os.Stderr, "PASS: p99 %.1f ms < %.0f ms over %d requests at %d clients\n", q(0.99), c.thresholdMS, len(lat), c.clients)
	return nil
}

// once is one iteration: bump the user's ratings_version (untimed), then time the request.
func once(s *site, c config, u *user, rng *rand.Rand) sample {
	u.mu.Lock()
	defer u.mu.Unlock()
	movie := u.movies[rng.IntN(len(u.movies))]
	v := halfSteps[2+rng.IntN(8)]
	if resp, data, err := s.do("PUT", fmt.Sprintf("/api/ratings/%d", movie), u.session, map[string]float64{"rating": v}); err != nil || resp.StatusCode != 200 {
		return sample{err: fmt.Errorf("rating PUT: %v %s", err, truncate(data))}
	}
	t0 := time.Now()
	resp, data, err := s.do("GET", fmt.Sprintf("/api/recommend?model=%s&k=%d", c.model, c.k), u.session, nil)
	elapsed := time.Since(t0)
	if err != nil {
		return sample{err: err}
	}
	if resp.StatusCode != 200 {
		return sample{err: fmt.Errorf("recommend %d %s", resp.StatusCode, truncate(data))}
	}
	var r struct {
		Model     string `json:"model"`
		ColdStart bool   `json:"cold_start"`
		Items     []json.RawMessage
	}
	if err := json.Unmarshal(data, &r); err != nil {
		return sample{err: err}
	}
	if r.ColdStart || r.Model != c.model || len(r.Items) == 0 || resp.Header.Get("X-Model-Version") == "" {
		return sample{err: fmt.Errorf("unexpected answer: model %s cold_start %v items %d version %q", r.Model, r.ColdStart, len(r.Items), resp.Header.Get("X-Model-Version"))}
	}
	return sample{elapsed: elapsed}
}

func truncate(b []byte) string {
	if len(b) > 200 {
		return string(b[:200])
	}
	return string(b)
}

func loadavg() string {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return ""
	}
	f := strings.Fields(string(b))
	if len(f) < 3 {
		return ""
	}
	return strings.Join(f[:3], " ")
}
