package reports

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/contractsdir"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// privateMarker is in every file a test puts where it must never be served.
const privateMarker = "PRIVATE-MARKER-must-never-be-served"

type clock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *clock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *clock) advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

// layout is a data directory with reports/ (the fixture reports) next to
// reports-private/ (per-user files and a full report that must stay private).
type layout struct {
	root, reports, private string
}

func newLayout(t *testing.T) layout {
	t.Helper()
	root := t.TempDir()
	l := layout{root: root, reports: filepath.Join(root, "reports"), private: filepath.Join(root, "reports-private")}
	for _, d := range []string{l.reports, l.private} {
		if err := os.Mkdir(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	src, err := contractsdir.Fixtures("reports")
	if err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(src)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		b, err := os.ReadFile(filepath.Join(src, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		write(t, filepath.Join(l.reports, e.Name()), b)
	}
	// Private files, each a well-formed report with a valid run_id.
	write(t, filepath.Join(l.private, "secret_run.json"), report("secret_run", privateMarker))
	write(t, filepath.Join(l.private, "2026-11-09T21-14-03Z_als.per_user.json"), report("2026-11-09T21-14-03Z_als_per_user", privateMarker))
	return l
}

func write(t *testing.T, path string, b []byte) {
	t.Helper()
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// report returns a minimal schema_version 1 report.
func report(runID, note string) []byte {
	b, _ := json.Marshal(map[string]any{
		"schema_version": 1, "run_id": runID, "created_at": "2026-11-10T00:00:00Z", "dataset": "ml-latest-small",
		"evaluated_on": "test", "model": "pop", "config": map[string]any{"note": note},
		"metrics":    map[string]any{"ndcg@10": map[string]any{"mean": 0.1, "ci95": []float64{0.09, 0.11}}, "recall@50": map[string]any{"mean": 0.2, "ci95": []float64{0.19, 0.21}}},
		"aggregates": map[string]any{"coverage@10": 0.01},
	})
	return b
}

type env struct {
	*cachetest.Env
	layout layout
	clock  *clock
}

func newEnv(t *testing.T, l layout) *env {
	t.Helper()
	c := &clock{t: time.Date(2026, 11, 10, 0, 0, 0, 0, time.UTC)}
	e := cachetest.NewEnv(t, nil, func(mux *http.ServeMux, d *app.Deps) {
		d.Config.ReportsDir = l.reports
		register(mux, d, c.now)
	})
	return &env{Env: e, layout: l, clock: c}
}

// do sends GET path through the full handler stack, following up to three
// same-server redirects (http.ServeMux redirects unclean paths).
func (e *env) do(path string) *httptest.ResponseRecorder {
	e.T.Helper()
	for i := 0; ; i++ {
		req := httptest.NewRequest("GET", path, nil)
		req.Host = cachetest.Host
		rec := httptest.NewRecorder()
		e.Handler.ServeHTTP(rec, req)
		if rec.Code/100 != 3 || i == 3 {
			return rec
		}
		path = rec.Header().Get("Location")
	}
}

// ---------------------------------------------------------------- contract

func TestContractFixtures(t *testing.T) {
	fixtures, err := contractsdir.Fixtures("")
	if err != nil {
		t.Fatal(err)
	}
	for _, op := range []string{"listReports", "getReport"} {
		for _, c := range cachetest.PublicCases(t, op) {
			t.Run(op+"/"+c.Name, func(t *testing.T) {
				e := newEnv(t, newLayout(t))
				rec := e.do(c.Request.Path)
				if rec.Code != c.Response.Status {
					t.Fatalf("status %d, want %d (%s)", rec.Code, c.Response.Status, rec.Body.String())
				}
				if rec.Header().Get("X-Request-ID") == "" {
					t.Error("no X-Request-ID")
				}
				if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
					t.Errorf("Content-Type %q", ct)
				}
				want := []byte(c.Response.Body)
				var ref struct {
					Ref string `json:"$fixtureRef"`
				}
				if json.Unmarshal(want, &ref) == nil && ref.Ref != "" {
					if want, err = os.ReadFile(filepath.Join(fixtures, filepath.FromSlash(ref.Ref))); err != nil {
						t.Fatal(err)
					}
				}
				if !jsonEqual(t, rec.Body.Bytes(), want) {
					t.Errorf("body\n got %s\nwant %s", rec.Body.String(), want)
				}
			})
		}
	}
}

func jsonEqual(t *testing.T, a, b []byte) bool {
	t.Helper()
	var x, y any
	if err := json.Unmarshal(a, &x); err != nil {
		t.Fatalf("body %q: %v", a, err)
	}
	if err := json.Unmarshal(b, &y); err != nil {
		t.Fatal(err)
	}
	xa, _ := json.Marshal(x)
	ya, _ := json.Marshal(y)
	return bytes.Equal(xa, ya)
}

// ---------------------------------------------------------------- security

// Gate: traversal attempts never escape the listing; reports-private is
// never readable.
func TestPathTraversal(t *testing.T) {
	l := newLayout(t)
	// Things placed inside reports/ that must still not be served.
	write(t, filepath.Join(l.reports, "leak.per_user.json"), report("leak", privateMarker))
	write(t, filepath.Join(l.reports, ".hidden.json"), report("hidden", privateMarker))
	if err := os.Symlink(filepath.Join(l.private, "secret_run.json"), filepath.Join(l.reports, "linked.json")); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(l.reports, "comparisons"), 0o755); err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(l.reports, "comparisons", "nested.json"), report("nested", privateMarker))
	e := newEnv(t, l)

	abs := filepath.Join(l.private, "secret_run.json")
	paths := []string{
		"/api/reports/..",
		"/api/reports/%2e%2e",
		"/api/reports/%2E%2E",
		"/api/reports/../reports-private/secret_run",
		"/api/reports/..%2freports-private%2fsecret_run",
		"/api/reports/%2e%2e%2freports-private%2fsecret_run",
		"/api/reports/%2e%2e%2f%2e%2e%2fetc%2fpasswd",
		"/api/reports/..%5creports-private%5csecret_run",
		"/api/reports/secret_run",
		"/api/reports/secret_run.json",
		"/api/reports/2026-11-09T21-14-03Z_als.per_user",
		"/api/reports/2026-11-09T21-14-03Z_als.per_user.json",
		"/api/reports/2026-11-09T21-14-03Z_als_per_user",
		"/api/reports/leak",
		"/api/reports/leak.per_user",
		"/api/reports/hidden",
		"/api/reports/.hidden",
		"/api/reports/linked",
		"/api/reports/nested",
		"/api/reports/comparisons%2fnested",
		"/api/reports/comparisons/nested",
		"/api/reports/" + strings.ReplaceAll(abs, "/", "%2f"),
		"/api/reports/%2fetc%2fpasswd",
		"/api/reports//etc/passwd",
		"/api/reports/als",
		"/api/reports/als.json",
		"/api/reports/2026-11-09T21-14-03Z_als.json",
		"/api/reports/2026-11-09T21-14-03Z_als%00",
		"/api/reports/" + strings.Repeat("a", 101),
		"/api/reports/2026-11-09T21-14-03Z_ALS",
		"/api/reports/%20",
	}
	for _, p := range paths {
		rec := e.do(p)
		if rec.Code != 400 && rec.Code != 404 {
			t.Errorf("%s: status %d (%s)", p, rec.Code, rec.Body.String())
		}
		if strings.Contains(rec.Body.String(), privateMarker) || strings.Contains(rec.Body.String(), "schema_version") {
			t.Errorf("%s served a report: %s", p, rec.Body.String())
		}
	}
	list := e.do("/api/reports")
	if list.Code != 200 {
		t.Fatalf("list: %d", list.Code)
	}
	for _, id := range []string{"secret_run", "leak", "hidden", "nested", "_per_user", privateMarker} {
		if strings.Contains(list.Body.String(), id) {
			t.Errorf("listing contains %q: %s", id, list.Body.String())
		}
	}
	if strings.Contains(e.Logs.String(), privateMarker) {
		t.Error("private content logged")
	}
}

// Gate: only IDs in the current listing are served, matched exactly. A
// file named like a run but describing another run_id isn't served under
// its file name; malformed, oversized and run_id-less files are left out.
func TestOnlyListedIDsServed(t *testing.T) {
	l := newLayout(t)
	write(t, filepath.Join(l.reports, "named_like_this.json"), report("actually_that", "x"))
	write(t, filepath.Join(l.reports, "bad_id.json"), report("bad id/../x", "x"))
	write(t, filepath.Join(l.reports, "no_id.json"), []byte(`{"schema_version":1}`))
	write(t, filepath.Join(l.reports, "not_json.json"), []byte(`{"schema_version":1, "run_id": "not_json"`))
	write(t, filepath.Join(l.reports, "array.json"), []byte(`[{"schema_version":1,"run_id":"array"}]`))
	write(t, filepath.Join(l.reports, "float_version.json"), []byte(`{"schema_version":1.5,"run_id":"float_version"}`))
	big := report("too_big", strings.Repeat("x", MaxReportBytes))
	write(t, filepath.Join(l.reports, "too_big.json"), big)
	write(t, filepath.Join(l.reports, "notes.txt"), report("txt_file", "x"))
	e := newEnv(t, l)

	if rec := e.do("/api/reports/actually_that"); rec.Code != 200 {
		t.Fatalf("listed id: %d", rec.Code)
	}
	for _, id := range []string{"named_like_this", "no_id", "not_json", "array", "float_version", "too_big", "txt_file", "notes"} {
		if rec := e.do("/api/reports/" + id); rec.Code != 404 {
			t.Errorf("%s: %d", id, rec.Code)
		}
	}
	var list struct {
		Items []struct {
			RunID string `json:"run_id"`
		} `json:"items"`
	}
	rec := e.do("/api/reports")
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil {
		t.Fatal(err)
	}
	if len(list.Items) != 9 {
		t.Fatalf("listing has %d items: %s", len(list.Items), rec.Body.String())
	}
}

// A file replaced after it was listed is served only if it still describes
// the same run.
func TestReplacedFileNotServedUnderOldID(t *testing.T) {
	l := newLayout(t)
	write(t, filepath.Join(l.reports, "r.json"), report("run_one", "x"))
	e := newEnv(t, l)
	if rec := e.do("/api/reports/run_one"); rec.Code != 200 {
		t.Fatalf("%d", rec.Code)
	}
	write(t, filepath.Join(l.reports, "r.json"), report("run_two", privateMarker))
	rec := e.do("/api/reports/run_one")
	if rec.Code != 404 || strings.Contains(rec.Body.String(), privateMarker) {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
}

// ---------------------------------------------------------------- listing

// The listing is cached for a few seconds, then re-read.
func TestListingCached(t *testing.T) {
	l := newLayout(t)
	e := newEnv(t, l)
	if rec := e.do("/api/reports"); rec.Code != 200 {
		t.Fatal(rec.Code)
	}
	write(t, filepath.Join(l.reports, "new.json"), report("new_run", "x"))
	if rec := e.do("/api/reports/new_run"); rec.Code != 404 {
		t.Fatalf("new report visible before the listing expired: %d", rec.Code)
	}
	e.clock.advance(listingTTL + time.Millisecond)
	if rec := e.do("/api/reports/new_run"); rec.Code != 200 {
		t.Fatalf("new report not visible after the listing expired: %d", rec.Code)
	}
	rec := e.do("/api/reports")
	if !strings.Contains(rec.Body.String(), `"run_id":"new_run"`) {
		t.Fatalf("listing: %s", rec.Body.String())
	}
	// Newest first: new_run (2026-11-10) leads.
	if !strings.HasPrefix(rec.Body.String(), `{"items":[{"run_id":"new_run"`) {
		t.Fatalf("order: %s", rec.Body.String()[:120])
	}
}

// A missing reports directory is an empty listing, not an error.
func TestMissingDirectory(t *testing.T) {
	e := newEnv(t, layout{reports: filepath.Join(t.TempDir(), "nope")})
	rec := e.do("/api/reports")
	if rec.Code != 200 || strings.TrimSpace(rec.Body.String()) != `{"items":[]}` {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if rec := e.do("/api/reports/anything"); rec.Code != 404 {
		t.Fatalf("%d", rec.Code)
	}
}

// Summaries of schema_version 1 reports missing headline pieces get a null
// headline rather than a half-filled one.
func TestSummaryPartialHeadline(t *testing.T) {
	l := layout{reports: t.TempDir()}
	write(t, filepath.Join(l.reports, "a.json"), []byte(`{"schema_version":1,"run_id":"a","model":"pop","dataset":"d","evaluated_on":"test","created_at":"2026-11-10T00:00:00Z","config":{},"metrics":{}}`))
	write(t, filepath.Join(l.reports, "b.json"), []byte(`{"schema_version":1,"run_id":"b","created_at":"not a time","evaluated_on":"bogus"}`))
	e := newEnv(t, l)
	rec := e.do("/api/reports")
	want := `{"items":[` +
		`{"run_id":"a","schema_version":1,"model":"pop","dataset":"d","evaluated_on":"test","created_at":"2026-11-10T00:00:00Z","config":{},"headline":null},` +
		`{"run_id":"b","schema_version":1,"model":null,"dataset":null,"evaluated_on":null,"created_at":null,"config":null,"headline":null}]}`
	if strings.TrimSpace(rec.Body.String()) != want {
		t.Fatalf("got  %s\nwant %s", rec.Body.String(), want)
	}
}
