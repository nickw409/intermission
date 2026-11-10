package reports

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// How run IDs map to files, and why it is safe.
//
// recs-api never builds a file path from request input. The listing reads
// the top level of RECS_REPORTS_DIR (no subdirectories, so
// data/reports-private and data/reports/comparisons are never visited),
// keeps regular *.json files only (no symlinks, no dotfiles, no
// *.per_user.json), parses each one and takes its run_id from the content.
// GET /api/reports/{runId} validates the ID against the contract pattern
// and then looks it up, by exact string match, in that listing; the file
// path served is the one the listing found. Before serving, the file is
// opened again, checked to be the same regular file, and re-parsed to make
// sure it still describes the requested run.

// Limits.
const (
	// MaxReportBytes caps the size of a report file that is listed or served.
	MaxReportBytes = 4 << 20
	// maxFiles caps how many directory entries one listing examines.
	maxFiles = 5000
	// listingTTL is how long a listing is reused before the directory is
	// read again.
	listingTTL = 5 * time.Second
	// knownSchemaVersion is the report schema recs-api can summarise
	// (contracts/report.schema.json).
	knownSchemaVersion = 1
)

// runIDPattern is the runId pattern in contracts/public.yaml.
var runIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,100}$`)

// Public errors. Messages match contracts/fixtures/public.
var (
	errBadRunID = apperr.New(http.StatusBadRequest, apperr.CodeBadRequest, "invalid run id")
	errNotFound = apperr.New(http.StatusNotFound, apperr.CodeNotFound, "report not found")
)

// Estimate is the Estimate schema.
type Estimate struct {
	Mean float64    `json:"mean"`
	CI95 [2]float64 `json:"ci95"`
}

// Headline is ReportSummary.headline.
type Headline struct {
	NDCG10     Estimate `json:"ndcg@10"`
	Recall50   Estimate `json:"recall@50"`
	Coverage10 float64  `json:"coverage@10"`
}

// Summary is the ReportSummary schema. For an unknown schema_version only
// RunID and SchemaVersion are set.
type Summary struct {
	RunID         string          `json:"run_id"`
	SchemaVersion int64           `json:"schema_version"`
	Model         *string         `json:"model"`
	Dataset       *string         `json:"dataset"`
	EvaluatedOn   *string         `json:"evaluated_on"`
	CreatedAt     *string         `json:"created_at"`
	Config        json.RawMessage `json:"config"` // a JSON object, or null
	Headline      *Headline       `json:"headline"`

	created time.Time // parsed CreatedAt, for ordering
}

// List is the ReportList schema.
type List struct {
	Items []Summary `json:"items"`
}

// Register adds this package's routes to mux. Called once from main.go.
//
//	GET /api/reports
//	GET /api/reports/{runId}
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, time.Now)
}

func register(mux *http.ServeMux, d *app.Deps, now func() time.Time) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	h := &handler{dir: d.Config.ReportsDir, now: now, logger: logger}
	mux.HandleFunc("GET /api/reports", h.list)
	mux.HandleFunc("GET /api/reports/{runId}", h.get)
}

type handler struct {
	dir    string
	now    func() time.Time
	logger *slog.Logger

	mu      sync.Mutex
	builtAt time.Time
	cached  *listing
}

// listing is one scan of the reports directory.
type listing struct {
	items []Summary         // newest first
	paths map[string]string // run_id -> file path found by the scan
}

func (h *handler) list(w http.ResponseWriter, r *http.Request) {
	l := h.listing(r)
	apperr.WriteJSON(w, http.StatusOK, List{Items: l.items})
}

func (h *handler) get(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("runId")
	if !runIDPattern.MatchString(id) {
		apperr.Write(w, errBadRunID)
		return
	}
	path, ok := h.listing(r).paths[id]
	if !ok {
		apperr.Write(w, errNotFound)
		return
	}
	body, err := readReport(path)
	if err != nil {
		h.logger.WarnContext(r.Context(), "reports: listed report unreadable", "request_id", reqid.FromContext(r.Context()), "file", filepath.Base(path), "err", err)
		apperr.Write(w, errNotFound)
		return
	}
	head, err := parseHead(body)
	if err != nil || head.runID != id {
		// Replaced since it was listed.
		apperr.Write(w, errNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

// listing returns the cached listing, rebuilding it once it is older than
// listingTTL.
func (h *handler) listing(r *http.Request) *listing {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()
	if h.cached != nil && now.Sub(h.builtAt) < listingTTL && !now.Before(h.builtAt) {
		return h.cached
	}
	h.cached = h.scan(r)
	h.builtAt = now
	return h.cached
}

// scan reads the top level of the reports directory.
func (h *handler) scan(r *http.Request) *listing {
	l := &listing{items: []Summary{}, paths: map[string]string{}}
	entries, err := os.ReadDir(h.dir)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			h.logger.WarnContext(r.Context(), "reports: cannot read reports directory", "request_id", reqid.FromContext(r.Context()), "err", err)
		}
		return l
	}
	if len(entries) > maxFiles {
		entries = entries[:maxFiles]
	}
	for _, e := range entries { // ReadDir sorts by name, so duplicates resolve deterministically
		name := e.Name()
		if !listable(name) || !e.Type().IsRegular() {
			continue
		}
		path := filepath.Join(h.dir, name)
		body, err := readReport(path)
		if err != nil {
			continue
		}
		head, err := parseHead(body)
		if err != nil {
			continue
		}
		if _, dup := l.paths[head.runID]; dup {
			h.logger.WarnContext(r.Context(), "reports: duplicate run_id; keeping the first file", "run_id", head.runID, "file", name)
			continue
		}
		l.paths[head.runID] = path
		l.items = append(l.items, summarize(head))
	}
	slices.SortStableFunc(l.items, newestFirst)
	return l
}

// listable reports whether a directory entry name may hold a public report.
func listable(name string) bool {
	return strings.HasSuffix(name, ".json") && !strings.HasPrefix(name, ".") && !strings.Contains(name, ".per_user")
}

// newestFirst orders by created_at descending; reports without a usable
// created_at go last; ties by run_id descending.
func newestFirst(a, b Summary) int {
	switch {
	case a.CreatedAt != nil && b.CreatedAt == nil:
		return -1
	case a.CreatedAt == nil && b.CreatedAt != nil:
		return 1
	}
	if c := b.created.Compare(a.created); c != 0 {
		return c
	}
	return strings.Compare(b.RunID, a.RunID)
}

// openRegular opens path for reading only if its last component is not a
// symlink (O_NOFOLLOW) and the opened file is a regular file (checked on the
// open descriptor, so nothing can be swapped in between). O_NONBLOCK keeps
// a FIFO from blocking the open; it is then refused as not regular.
func openRegular(path string) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	if !info.Mode().IsRegular() {
		f.Close()
		return nil, errors.New("not a regular file")
	}
	return f, nil
}

// readReport reads path with openRegular, refusing files over
// MaxReportBytes.
func readReport(path string) ([]byte, error) {
	f, err := openRegular(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	after, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if after.Size() > MaxReportBytes {
		return nil, errors.New("report too large")
	}
	body, err := io.ReadAll(io.LimitReader(f, MaxReportBytes+1))
	if err != nil {
		return nil, err
	}
	if len(body) > MaxReportBytes {
		return nil, errors.New("report too large")
	}
	return body, nil
}

// head is the parsed top level of a report.
type head struct {
	runID         string
	schemaVersion int64
	fields        map[string]json.RawMessage
}

// parseHead checks that body is one JSON object with an integer
// schema_version and a run_id matching the contract pattern.
func parseHead(body []byte) (head, error) {
	var h head
	dec := json.NewDecoder(bytes.NewReader(body))
	if err := dec.Decode(&h.fields); err != nil || h.fields == nil {
		return h, errors.New("not a JSON object")
	}
	if _, err := dec.Token(); err != io.EOF {
		return h, errors.New("trailing data")
	}
	if err := json.Unmarshal(h.fields["schema_version"], &h.schemaVersion); err != nil {
		return h, fmt.Errorf("schema_version: %w", err)
	}
	if err := json.Unmarshal(h.fields["run_id"], &h.runID); err != nil || !runIDPattern.MatchString(h.runID) {
		return h, errors.New("missing or invalid run_id")
	}
	return h, nil
}

// summarize builds a ReportSummary. Fields of a schema_version 1 report that
// are missing or malformed are null; for any other version everything but
// run_id and schema_version is null.
func summarize(h head) Summary {
	s := Summary{RunID: h.runID, SchemaVersion: h.schemaVersion, Config: json.RawMessage("null")}
	if h.schemaVersion != knownSchemaVersion {
		return s
	}
	s.Model = stringField(h.fields["model"])
	s.Dataset = stringField(h.fields["dataset"])
	if on := stringField(h.fields["evaluated_on"]); on != nil && (*on == "test" || *on == "validation") {
		s.EvaluatedOn = on
	}
	if at := stringField(h.fields["created_at"]); at != nil {
		if t, err := time.Parse(time.RFC3339, *at); err == nil {
			s.CreatedAt, s.created = at, t
		}
	}
	if cfg := bytes.TrimSpace(h.fields["config"]); len(cfg) > 0 && cfg[0] == '{' {
		s.Config = json.RawMessage(cfg)
	}
	s.Headline = headline(h.fields)
	return s
}

func stringField(raw json.RawMessage) *string {
	var s string
	if raw == nil || json.Unmarshal(raw, &s) != nil {
		return nil
	}
	return &s
}

// headline takes ndcg@10 and recall@50 from metrics and coverage@10 from
// aggregates; nil unless all three are present and well formed.
func headline(fields map[string]json.RawMessage) *Headline {
	var metrics struct {
		NDCG10   *estimateIn `json:"ndcg@10"`
		Recall50 *estimateIn `json:"recall@50"`
	}
	var aggregates struct {
		Coverage10 *float64 `json:"coverage@10"`
	}
	if json.Unmarshal(fields["metrics"], &metrics) != nil || json.Unmarshal(fields["aggregates"], &aggregates) != nil {
		return nil
	}
	if !metrics.NDCG10.ok() || !metrics.Recall50.ok() || aggregates.Coverage10 == nil {
		return nil
	}
	return &Headline{NDCG10: metrics.NDCG10.estimate(), Recall50: metrics.Recall50.estimate(), Coverage10: *aggregates.Coverage10}
}

type estimateIn struct {
	Mean *float64  `json:"mean"`
	CI95 []float64 `json:"ci95"`
}

func (e *estimateIn) ok() bool { return e != nil && e.Mean != nil && len(e.CI95) == 2 }

func (e *estimateIn) estimate() Estimate {
	return Estimate{Mean: *e.Mean, CI95: [2]float64{e.CI95[0], e.CI95[1]}}
}
