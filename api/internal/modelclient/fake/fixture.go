package fake

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"

	"github.com/nickw409/intermission/api/internal/modelclient"
)

// Fixture is one contracts/fixtures/model/<operationId>.json file.
type Fixture struct {
	OperationID string `json:"operationId"`
	Cases       []Case `json:"cases"`
}

// Case is one request/response example for an operation.
type Case struct {
	Name     string   `json:"name"`
	Request  Request  `json:"request"`
	Response Response `json:"response"`
}

// Request is the request half of a Case. Query values may be JSON strings,
// numbers or booleans; they are compared as their string form. RawBody, when
// set, is the exact request body text (used for invalid_json cases) and
// replaces Body.
type Request struct {
	Method  string                     `json:"method"`
	Path    string                     `json:"path"`
	Query   map[string]json.RawMessage `json:"query,omitempty"`
	Headers map[string]string          `json:"headers,omitempty"`
	Body    json.RawMessage            `json:"body,omitempty"`
	RawBody *string                    `json:"rawBody,omitempty"`
}

// BodyBytes returns the request body as sent on the wire.
func (r Request) BodyBytes() []byte {
	if r.RawBody != nil {
		return []byte(*r.RawBody)
	}
	return r.Body
}

// Response is the response half of a Case.
type Response struct {
	Status  int               `json:"status"`
	Headers map[string]string `json:"headers,omitempty"`
	Body    json.RawMessage   `json:"body,omitempty"`
}

// Header returns the response header named key (case-insensitive), or "".
func (r Response) Header(key string) string {
	for k, v := range r.Headers {
		if strings.EqualFold(k, key) {
			return v
		}
	}
	return ""
}

// Success reports whether the response status is 2xx.
func (r Response) Success() bool { return r.Status >= 200 && r.Status < 300 }

// op describes one operation in contracts/model.yaml.
type op struct {
	method string
	path   string // documentation only; routing is done by route()
	// normalize strictly decodes a request body into the operation's request
	// type and re-encodes it, so that equivalent bodies compare equal. nil
	// for operations without a request body.
	normalize func([]byte) ([]byte, error)
}

// Operation IDs in contracts/model.yaml.
const (
	OpRecommend   = "recommend"
	OpCompare     = "compare"
	OpSimilar     = "similar"
	OpOnboarding  = "onboarding"
	OpSearch      = "search"
	OpMoviesBatch = "moviesBatch"
	OpVersion     = "version"
	OpModels      = "models"
	OpHealthz     = "healthz"
	OpMetrics     = "metrics"
)

var ops = map[string]op{
	OpRecommend:   {"POST", "/v1/recommend", normalizer[modelclient.RecommendRequest]},
	OpCompare:     {"POST", "/v1/compare", normalizer[modelclient.CompareRequest]},
	OpSimilar:     {"GET", "/v1/similar/{movie_id}", nil},
	OpOnboarding:  {"POST", "/v1/onboarding", normalizer[modelclient.OnboardingRequest]},
	OpSearch:      {"GET", "/v1/movies/search", nil},
	OpMoviesBatch: {"POST", "/v1/movies/batch", normalizer[modelclient.BatchRequest]},
	OpVersion:     {"GET", "/v1/version", nil},
	OpModels:      {"GET", "/v1/models", nil},
	OpHealthz:     {"GET", "/healthz", nil},
	OpMetrics:     {"GET", "/metrics", nil},
}

// OperationIDs returns every operationId in contracts/model.yaml, sorted.
func OperationIDs() []string {
	ids := make([]string, 0, len(ops))
	for id := range ops {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	return ids
}

// route maps an HTTP method and path to an operationId.
func route(method, path string) (string, bool) {
	if rest, ok := strings.CutPrefix(path, "/v1/similar/"); ok {
		if method == "GET" && rest != "" && !strings.Contains(rest, "/") {
			return OpSimilar, true
		}
		return "", false
	}
	for id, o := range ops {
		if id != OpSimilar && o.method == method && o.path == path {
			return id, true
		}
	}
	return "", false
}

// normalizer strictly decodes b into T and re-encodes it.
func normalizer[T any](b []byte) ([]byte, error) {
	var v T
	if err := decodeStrict(b, &v); err != nil {
		return nil, err
	}
	return json.Marshal(v)
}

// decodeStrict decodes exactly one JSON value into v, rejecting unknown fields
// and trailing data.
func decodeStrict(b []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return err
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return errors.New("trailing data after JSON value")
	}
	return nil
}

// ParseFixture parses and validates one fixture file.
func ParseFixture(data []byte) (*Fixture, error) {
	var f Fixture
	if err := decodeStrict(data, &f); err != nil {
		return nil, fmt.Errorf("parse fixture: %w", err)
	}
	if _, ok := ops[f.OperationID]; !ok {
		return nil, fmt.Errorf("fixture: unknown operationId %q", f.OperationID)
	}
	if len(f.Cases) == 0 {
		return nil, fmt.Errorf("fixture %s: no cases", f.OperationID)
	}
	seen := map[string]bool{}
	for i := range f.Cases {
		c := &f.Cases[i]
		if c.Name == "" {
			return nil, fmt.Errorf("fixture %s: case %d has no name", f.OperationID, i)
		}
		if seen[c.Name] {
			return nil, fmt.Errorf("fixture %s: duplicate case name %q", f.OperationID, c.Name)
		}
		seen[c.Name] = true
		if c.Request.Method == "" || !strings.HasPrefix(c.Request.Path, "/") {
			return nil, fmt.Errorf("fixture %s/%s: request needs a method and an absolute path", f.OperationID, c.Name)
		}
		c.Request.Method = strings.ToUpper(c.Request.Method)
		if c.Request.RawBody != nil && len(c.Request.Body) > 0 {
			return nil, fmt.Errorf("fixture %s/%s: request has both body and rawBody", f.OperationID, c.Name)
		}
		if c.Response.Status < 100 || c.Response.Status > 599 {
			return nil, fmt.Errorf("fixture %s/%s: bad response status %d", f.OperationID, c.Name, c.Response.Status)
		}
		// Allow "?a=b" inside path as well as the query object.
		if p, q, ok := strings.Cut(c.Request.Path, "?"); ok {
			vals, err := url.ParseQuery(q)
			if err != nil {
				return nil, fmt.Errorf("fixture %s/%s: bad query in path: %w", f.OperationID, c.Name, err)
			}
			c.Request.Path = p
			if c.Request.Query == nil {
				c.Request.Query = map[string]json.RawMessage{}
			}
			for k := range vals {
				b, _ := json.Marshal(vals.Get(k))
				c.Request.Query[k] = b
			}
		}
		if _, err := queryStrings(c.Request.Query); err != nil {
			return nil, fmt.Errorf("fixture %s/%s: %w", f.OperationID, c.Name, err)
		}
	}
	return &f, nil
}

// LoadDir loads every *.json fixture in dir, keyed by operationId. Each file
// must be named <operationId>.json.
func LoadDir(dir string) (map[string]*Fixture, error) {
	paths, err := filepath.Glob(filepath.Join(dir, "*.json"))
	if err != nil {
		return nil, err
	}
	out := make(map[string]*Fixture, len(paths))
	for _, p := range paths {
		data, err := os.ReadFile(p)
		if err != nil {
			return nil, err
		}
		f, err := ParseFixture(data)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", filepath.Base(p), err)
		}
		if want := f.OperationID + ".json"; filepath.Base(p) != want {
			return nil, fmt.Errorf("%s: operationId %q must live in %s", filepath.Base(p), f.OperationID, want)
		}
		out[f.OperationID] = f
	}
	return out, nil
}

// queryStrings converts fixture query values to strings.
func queryStrings(q map[string]json.RawMessage) (map[string]string, error) {
	out := make(map[string]string, len(q))
	for k, raw := range q {
		var s string
		if err := json.Unmarshal(raw, &s); err == nil {
			out[k] = s
			continue
		}
		var v any
		if err := json.Unmarshal(raw, &v); err != nil {
			return nil, fmt.Errorf("query %q: %w", k, err)
		}
		switch v.(type) {
		case float64, bool:
			out[k] = strings.TrimSpace(string(raw))
		default:
			return nil, fmt.Errorf("query %q: value must be a string, number or boolean", k)
		}
	}
	return out, nil
}

// matches reports whether an actual request matches the case's request.
func (c *Case) matches(o op, method, path string, query url.Values, body []byte) bool {
	if c.Request.Method != method || c.Request.Path != path {
		return false
	}
	want, err := queryStrings(c.Request.Query)
	if err != nil || len(want) != len(query) {
		return false
	}
	for k, v := range want {
		got, ok := query[k]
		if !ok || len(got) != 1 || got[0] != v {
			return false
		}
	}
	if c.Request.RawBody != nil {
		return strings.TrimSpace(*c.Request.RawBody) == string(bytes.TrimSpace(body))
	}
	return bodyMatches(o, c.Request.Body, body)
}

func bodyMatches(o op, fixture json.RawMessage, actual []byte) bool {
	fixture = bytes.TrimSpace(fixture)
	actual = bytes.TrimSpace(actual)
	if len(fixture) == 0 || bytes.Equal(fixture, []byte("null")) {
		return len(actual) == 0
	}
	if len(actual) == 0 {
		return false
	}
	if jsonEqual(fixture, actual) {
		return true
	}
	if o.normalize == nil {
		return false
	}
	a, errA := o.normalize(fixture)
	b, errB := o.normalize(actual)
	return errA == nil && errB == nil && bytes.Equal(a, b)
}

// jsonEqual reports whether a and b decode to deeply equal values.
func jsonEqual(a, b []byte) bool {
	var va, vb any
	if json.Unmarshal(a, &va) != nil || json.Unmarshal(b, &vb) != nil {
		return false
	}
	return reflect.DeepEqual(va, vb)
}
