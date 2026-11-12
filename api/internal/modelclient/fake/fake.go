package fake

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"math"
	"net/url"
	"os"
	"slices"
	"strconv"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/contractsdir"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// Call records one request the fake received.
type Call struct {
	OperationID string
	Method      string
	Path        string
	Query       url.Values
	Body        []byte
	RequestID   string // X-Request-ID header (HTTP) or reqid.FromContext (in-memory)
	Case        string // name of the case that answered; "" if none
}

// Fake is an in-memory modelclient.Client backed by fixture files. It is safe
// for concurrent use.
type Fake struct {
	mu       sync.Mutex
	fixtures map[string]*Fixture
	forced   map[string]string
	down     bool
	latency  time.Duration
	calls    []Call
}

var _ modelclient.Client = (*Fake)(nil)

// New loads the fixtures from contracts/fixtures/model, located with
// contractsdir.Find (RECS_CONTRACTS_DIR, else walking up from the working
// directory).
func New() (*Fake, error) {
	dir, err := contractsdir.Fixtures("model")
	if err != nil {
		return nil, err
	}
	if _, err := os.Stat(dir); err != nil {
		return nil, fmt.Errorf("model fixtures: %w", err)
	}
	return NewFromDir(dir)
}

// NewFromDir loads every fixture in dir.
func NewFromDir(dir string) (*Fake, error) {
	fx, err := LoadDir(dir)
	if err != nil {
		return nil, err
	}
	return NewFromFixtures(fx), nil
}

// NewFromFixtures builds a Fake from already-loaded fixtures.
func NewFromFixtures(fx map[string]*Fixture) *Fake {
	return &Fake{fixtures: maps.Clone(fx), forced: map[string]string{}}
}

// Fixtures returns the loaded fixtures keyed by operationId. Callers must not
// modify them.
func (f *Fake) Fixtures() map[string]*Fixture {
	f.mu.Lock()
	defer f.mu.Unlock()
	return maps.Clone(f.fixtures)
}

// SetDown makes every call fail with ErrUnavailable (HTTP: 503 model_unavailable).
func (f *Fake) SetDown(down bool) {
	f.mu.Lock()
	f.down = down
	f.mu.Unlock()
}

// SetLatency delays every call by d. A context that ends first makes the
// in-memory call fail with ErrUnavailable.
func (f *Fake) SetLatency(d time.Duration) {
	f.mu.Lock()
	f.latency = d
	f.mu.Unlock()
}

// Force makes every request for operationID answer with the named case,
// regardless of the request. An empty caseName clears it.
func (f *Fake) Force(operationID, caseName string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if caseName == "" {
		delete(f.forced, operationID)
		return nil
	}
	fx, ok := f.fixtures[operationID]
	if !ok {
		return fmt.Errorf("fake: no fixture for %s", operationID)
	}
	for _, c := range fx.Cases {
		if c.Name == caseName {
			f.forced[operationID] = caseName
			return nil
		}
	}
	return fmt.Errorf("fake: %s has no case %q", operationID, caseName)
}

// Calls returns a copy of every call received so far.
func (f *Fake) Calls() []Call {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]Call(nil), f.calls...)
}

// ResetCalls clears the call log.
func (f *Fake) ResetCalls() {
	f.mu.Lock()
	f.calls = nil
	f.mu.Unlock()
}

// errDown is returned by resolve when the fake is down.
var errDown = errors.New("fake: down")

// resolve waits for the configured latency, records the call and picks the
// answering case. It returns errDown when the fake is down and ctx.Err() if
// ctx ends during the delay.
func (f *Fake) resolve(ctx context.Context, opID, method, path string, query url.Values, body []byte, requestID string) (*Case, error) {
	f.mu.Lock()
	latency := f.latency
	f.mu.Unlock()
	if latency > 0 {
		t := time.NewTimer(latency)
		select {
		case <-t.C:
		case <-ctx.Done():
			t.Stop()
			return nil, ctx.Err()
		}
	}

	f.mu.Lock()
	defer f.mu.Unlock()
	call := Call{OperationID: opID, Method: method, Path: path, Query: query, Body: body, RequestID: requestID}
	defer func() { f.calls = append(f.calls, call) }()
	if f.down {
		return nil, errDown
	}
	fx, ok := f.fixtures[opID]
	if !ok {
		return nil, fmt.Errorf("fake: no fixture for %s", opID)
	}
	if name, ok := f.forced[opID]; ok {
		for i := range fx.Cases {
			if fx.Cases[i].Name == name {
				call.Case = name
				return &fx.Cases[i], nil
			}
		}
	}
	o := ops[opID]
	for i := range fx.Cases {
		if fx.Cases[i].matches(o, method, path, query, body) {
			call.Case = fx.Cases[i].Name
			return &fx.Cases[i], nil
		}
	}
	if opID == OpMoviesBatch {
		c := batchFromCatalog(fx, body)
		call.Case = c.Name
		return c, nil
	}
	for i := range fx.Cases {
		if fx.Cases[i].Response.Success() {
			call.Case = fx.Cases[i].Name
			return &fx.Cases[i], nil
		}
	}
	return nil, fmt.Errorf("fake: %s has no matching case and no success case", opID)
}

// do runs one in-memory call and decodes the case's response into T.
func do[T any](f *Fake, ctx context.Context, opID string, path string, query url.Values, reqBody any) (T, modelclient.Meta, error) {
	var zero T
	var body []byte
	if reqBody != nil {
		var err error
		if body, err = json.Marshal(reqBody); err != nil {
			return zero, modelclient.Meta{}, err
		}
	}
	if query == nil {
		query = url.Values{}
	}
	c, err := f.resolve(ctx, opID, ops[opID].method, path, query, body, reqid.FromContext(ctx))
	if err != nil {
		if errors.Is(err, errDown) || errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return zero, modelclient.Meta{}, fmt.Errorf("%w: %v", modelclient.ErrUnavailable, err)
		}
		return zero, modelclient.Meta{}, err
	}
	meta := modelclient.Meta{ModelVersion: c.Response.Header("X-Model-Version")}
	switch {
	case c.Response.Success():
		var out T
		if err := decodeStrict(c.Response.Body, &out); err != nil {
			return zero, meta, fmt.Errorf("fake: %s/%s response does not match the client type: %w", opID, c.Name, err)
		}
		return out, meta, nil
	case c.Response.Status == 503:
		return zero, meta, fmt.Errorf("%w: %s/%s", modelclient.ErrUnavailable, opID, c.Name)
	default:
		var eb modelclient.ErrorBody
		_ = json.Unmarshal(c.Response.Body, &eb)
		return zero, meta, &modelclient.StatusError{Status: c.Response.Status, Code: eb.Error.Code, Message: eb.Error.Message}
	}
}

// Recommend implements modelclient.Client.
func (f *Fake) Recommend(ctx context.Context, req modelclient.RecommendRequest) (modelclient.RecommendResponse, modelclient.Meta, error) {
	return do[modelclient.RecommendResponse](f, ctx, OpRecommend, "/v1/recommend", nil, req)
}

// Compare implements modelclient.Client.
func (f *Fake) Compare(ctx context.Context, req modelclient.CompareRequest) (modelclient.CompareResponse, modelclient.Meta, error) {
	return do[modelclient.CompareResponse](f, ctx, OpCompare, "/v1/compare", nil, req)
}

// Similar implements modelclient.Client.
func (f *Fake) Similar(ctx context.Context, movieID int64, model modelclient.ModelName, k int) (modelclient.SimilarResponse, modelclient.Meta, error) {
	q := url.Values{}
	if model != "" {
		q.Set("model", string(model))
	}
	if k != 0 {
		q.Set("k", strconv.Itoa(k))
	}
	return do[modelclient.SimilarResponse](f, ctx, OpSimilar, "/v1/similar/"+strconv.FormatInt(movieID, 10), q, nil)
}

// Onboarding implements modelclient.Client.
func (f *Fake) Onboarding(ctx context.Context, req modelclient.OnboardingRequest) (modelclient.IDList, modelclient.Meta, error) {
	return do[modelclient.IDList](f, ctx, OpOnboarding, "/v1/onboarding", nil, req)
}

// Search implements modelclient.Client.
func (f *Fake) Search(ctx context.Context, q string, limit int) (modelclient.IDList, modelclient.Meta, error) {
	vals := url.Values{"q": {q}}
	if limit != 0 {
		vals.Set("limit", strconv.Itoa(limit))
	}
	return do[modelclient.IDList](f, ctx, OpSearch, "/v1/movies/search", vals, nil)
}

// MoviesBatch implements modelclient.Client.
func (f *Fake) MoviesBatch(ctx context.Context, ids []int64) (modelclient.BatchResponse, modelclient.Meta, error) {
	return do[modelclient.BatchResponse](f, ctx, OpMoviesBatch, "/v1/movies/batch", nil, modelclient.BatchRequest{IDs: ids})
}

// Version implements modelclient.Client.
func (f *Fake) Version(ctx context.Context) (modelclient.VersionResponse, modelclient.Meta, error) {
	return do[modelclient.VersionResponse](f, ctx, OpVersion, "/v1/version", nil, nil)
}

// Models implements modelclient.Client.
func (f *Fake) Models(ctx context.Context) (modelclient.ModelsResponse, modelclient.Meta, error) {
	return do[modelclient.ModelsResponse](f, ctx, OpModels, "/v1/models", nil, nil)
}

// Healthz implements modelclient.Client.
func (f *Fake) Healthz(ctx context.Context) error {
	_, _, err := do[modelclient.Health](f, ctx, OpHealthz, "/healthz", nil, nil)
	return err
}

// CatalogCase names the synthesized movies/batch answer (see batchFromCatalog).
const CatalogCase = "catalog"

// maxMovieID is the largest movie ID recs-model accepts (contracts/model.yaml).
const maxMovieID = 2147483647

// batchFromCatalog answers a movies/batch request that no case matches. The
// body is checked as recs-model checks it (model-service batch_route): one to
// MaxBatchIDs IDs, each an integer in [1, 2147483647], else a 400 case. A valid
// request is answered from the catalog the fixtures describe: every movie in
// any 2xx moviesBatch case. Known IDs come back in request order, the rest in
// missing, as recs-model answers.
func batchFromCatalog(fx *Fixture, body []byte) *Case {
	catalog := map[int64]modelclient.MovieMeta{}
	version := ""
	for _, c := range fx.Cases {
		if !c.Response.Success() {
			continue
		}
		var resp modelclient.BatchResponse
		if json.Unmarshal(c.Response.Body, &resp) != nil {
			continue
		}
		for _, m := range resp.Movies {
			catalog[m.MovieID] = m
		}
		if version == "" {
			version = c.Response.Header("X-Model-Version")
		}
	}
	answer := func(status int, v any) *Case {
		b, _ := json.Marshal(v) // plain structs of strings and numbers: cannot fail
		return &Case{Name: CatalogCase, Request: Request{Method: "POST", Path: "/v1/movies/batch", Body: body},
			Response: Response{Status: status, Headers: map[string]string{"X-Model-Version": version}, Body: b}}
	}
	ids, code, msg := parseBatchIDs(body)
	if code != "" {
		return answer(400, modelclient.ErrorBody{Error: modelclient.ErrorDetail{Code: code, Message: msg}})
	}
	out := modelclient.BatchResponse{Movies: []modelclient.MovieMeta{}, Missing: []int64{}}
	for _, id := range ids {
		if m, ok := catalog[id]; ok {
			out.Movies = append(out.Movies, m)
		} else {
			out.Missing = append(out.Missing, id)
		}
	}
	return answer(200, out)
}

// parseBatchIDs validates a movies/batch body the way recs-model does
// (model-service parse_object, check_fields and batch_route) and returns its
// IDs, or the error code and message recs-model would answer with.
func parseBatchIDs(body []byte) (ids []int64, code, msg string) {
	const badJSON, badRequest = "invalid_json", "bad_request"
	if !utf8.Valid(body) || !nestingOK(body) || !json.Valid(body) {
		return nil, badJSON, "request body is not valid JSON"
	}
	var obj map[string]json.RawMessage
	if json.Unmarshal(body, &obj) != nil || obj == nil {
		return nil, badRequest, "request body must be a JSON object"
	}
	// recs-model reports the first unexpected field in key order.
	for _, key := range slices.Sorted(maps.Keys(obj)) {
		if key != "ids" {
			return nil, badRequest, "unexpected field" + echoToken(key)
		}
	}
	raw, ok := obj["ids"]
	if !ok {
		return nil, badRequest, `missing field "ids"`
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	var list []any
	if dec.Decode(&list) != nil || len(list) == 0 || len(list) > modelclient.MaxBatchIDs {
		return nil, badRequest, "ids must contain 1-200 movie ids"
	}
	for _, v := range list {
		id, ok := movieID(v)
		if !ok {
			return nil, badRequest, "movie_id must be an integer between 1 and 2147483647"
		}
		ids = append(ids, id)
	}
	return ids, "", ""
}

// maxJSONDepth is recs-model's Limits::kMaxJsonDepth.
const maxJSONDepth = 8

// nestingOK reports whether no array or object in s nests deeper than
// maxJSONDepth and there are never more closers than openers, as
// model-service nesting_ok does before parsing.
func nestingOK(s []byte) bool {
	depth := 0
	inString, escaped := false, false
	for _, c := range s {
		if inString {
			switch {
			case escaped:
				escaped = false
			case c == '\\':
				escaped = true
			case c == '"':
				inString = false
			}
			continue
		}
		switch c {
		case '"':
			inString = true
		case '[', '{':
			if depth++; depth > maxJSONDepth {
				return false
			}
		case ']', '}':
			if depth--; depth < 0 {
				return false
			}
		}
	}
	return true
}

// echoToken quotes a caller-supplied token for an error message only if it is
// at most 40 bytes of printable ASCII, as model-service echo_token does.
func echoToken(s string) string {
	if len(s) > 40 {
		return ""
	}
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] >= 0x7f {
			return ""
		}
	}
	return ` "` + s + `"`
}

// movieID reports whether v is a JSON integer (3.0 counts, as in JSON Schema)
// in [1, maxMovieID].
func movieID(v any) (int64, bool) {
	n, ok := v.(json.Number)
	if !ok {
		return 0, false
	}
	if i, err := n.Int64(); err == nil {
		return i, i >= 1 && i <= maxMovieID
	}
	f, err := n.Float64()
	if err != nil || f != math.Trunc(f) || f < 1 || f > maxMovieID {
		return 0, false
	}
	return int64(f), true
}
