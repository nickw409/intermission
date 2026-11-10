package modelclient

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/nickw409/intermission/api/internal/reqid"
)

// DefaultTimeout is the per-call deadline recs-api applies to recs-model calls.
const DefaultTimeout = 500 * time.Millisecond

// MaxResponseBytes caps how much of a recs-model response is read.
const MaxResponseBytes = 4 << 20

// HTTPConfig configures the real HTTP client.
type HTTPConfig struct {
	// SocketPath is the Unix socket recs-model listens on. Used when Addr is empty.
	SocketPath string
	// Addr is a TCP host:port fallback (dev). When set it takes precedence.
	Addr string
	// Timeout is the per-call deadline; zero means DefaultTimeout.
	Timeout time.Duration
	// Logger receives client-side errors; nil means slog.Default().
	Logger *slog.Logger
}

// NewHTTP returns the real recs-model client.
//
// Behaviour (spec: "Internal model API", rules for both sides):
//
//   - Every call has one deadline (Timeout, default 500 ms) applied through
//     the context; it covers connecting, the retry and reading the body. A
//     shorter deadline already on the caller's context wins.
//   - GET requests (reads, idempotent) are retried once, and only when the
//     first attempt failed to get any response for a reason other than the
//     deadline (dial refused, connection reset or closed before a response).
//     POST requests are never retried, and nothing is retried after a
//     response arrives.
//   - A valid X-Request-ID from the context (package reqid) is forwarded.
//   - Connection errors, timeouts and 503 responses are ErrUnavailable. Any
//     other non-2xx response is a *StatusError decoded from the Error body.
//   - 2xx bodies are decoded strictly (unknown fields and trailing data are
//     errors) and capped at MaxResponseBytes. A malformed response is a plain
//     error: recs-model answered, but not per contract.
func NewHTTP(cfg HTTPConfig) Client {
	if cfg.Timeout <= 0 {
		cfg.Timeout = DefaultTimeout
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	dialer := &net.Dialer{Timeout: cfg.Timeout}
	network, address, base := "unix", cfg.SocketPath, "http://recs-model"
	if cfg.Addr != "" {
		network, address, base = "tcp", cfg.Addr, "http://"+cfg.Addr
	}
	transport := &http.Transport{
		Proxy: nil, // never route the internal API through an environment proxy
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			return dialer.DialContext(ctx, network, address)
		},
		DisableCompression:  true,
		MaxIdleConns:        64,
		MaxIdleConnsPerHost: 64,
		IdleConnTimeout:     90 * time.Second,
	}
	return &httpClient{
		cfg:  cfg,
		base: base,
		http: &http.Client{
			Transport: transport,
			// recs-model never redirects; a redirect is surfaced as a StatusError.
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
		},
	}
}

type httpClient struct {
	cfg  HTTPConfig
	base string
	http *http.Client
}

var _ Client = (*httpClient)(nil)

// call performs one recs-model request and decodes a 2xx body into out (if
// out is non-nil). query may be nil; body is JSON-encoded when non-nil.
func (c *httpClient) call(ctx context.Context, method, path string, query url.Values, body any, out any) (Meta, error) {
	ctx, cancel := context.WithTimeout(ctx, c.cfg.Timeout)
	defer cancel()

	var payload []byte
	if body != nil {
		var err error
		if payload, err = json.Marshal(body); err != nil {
			return Meta{}, fmt.Errorf("recs-model %s %s: encode request: %w", method, path, err)
		}
	}
	u := c.base + path
	if len(query) > 0 {
		u += "?" + query.Encode()
	}

	resp, err := c.do(ctx, method, u, payload)
	if err != nil && method == http.MethodGet && isConnError(ctx, err) {
		resp, err = c.do(ctx, method, u, payload)
	}
	if err != nil {
		c.cfg.Logger.DebugContext(ctx, "recs-model unreachable", "method", method, "path", path,
			"request_id", reqid.FromContext(ctx), "err", err)
		return Meta{}, fmt.Errorf("%w: %s %s: %v", ErrUnavailable, method, path, err)
	}
	defer resp.Body.Close()

	meta := Meta{ModelVersion: resp.Header.Get("X-Model-Version")}
	data, err := io.ReadAll(io.LimitReader(resp.Body, MaxResponseBytes+1))
	if err != nil {
		// The connection broke or the deadline hit while reading the body.
		return meta, fmt.Errorf("%w: %s %s: reading response: %v", ErrUnavailable, method, path, err)
	}
	if len(data) > MaxResponseBytes {
		c.cfg.Logger.WarnContext(ctx, "recs-model response too large", "method", method, "path", path,
			"request_id", reqid.FromContext(ctx))
		return meta, fmt.Errorf("recs-model %s %s: response exceeds %d bytes", method, path, MaxResponseBytes)
	}

	switch {
	case resp.StatusCode == http.StatusServiceUnavailable:
		return meta, fmt.Errorf("%w: %s %s: 503", ErrUnavailable, method, path)
	case resp.StatusCode < 200 || resp.StatusCode > 299:
		se := &StatusError{Status: resp.StatusCode}
		var eb ErrorBody
		if json.Unmarshal(data, &eb) == nil {
			se.Code, se.Message = eb.Error.Code, eb.Error.Message
		}
		return meta, se
	}
	if out == nil {
		return meta, nil
	}
	if err := decodeStrict(data, out); err != nil {
		c.cfg.Logger.WarnContext(ctx, "recs-model response does not match the contract", "method", method,
			"path", path, "request_id", reqid.FromContext(ctx), "err", err)
		return meta, fmt.Errorf("recs-model %s %s: decode response: %w", method, path, err)
	}
	return meta, nil
}

// do sends one attempt.
func (c *httpClient) do(ctx context.Context, method, u string, payload []byte) (*http.Response, error) {
	var rd io.Reader
	if payload != nil {
		rd = bytes.NewReader(payload)
	}
	req, err := http.NewRequestWithContext(ctx, method, u, rd)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	if payload != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if id := reqid.FromContext(ctx); reqid.Valid(id) {
		req.Header.Set(reqid.Header, id)
	}
	resp, err := c.http.Do(req)
	if err != nil {
		// *url.Error embeds the full request URL, which carries the search
		// query. Keep only the underlying cause so it never reaches logs or
		// callers (callers already know the method and path).
		var ue *url.Error
		if errors.As(err, &ue) {
			err = ue.Err
		}
		return nil, err
	}
	return resp, nil
}

// isConnError reports whether err (from http.Client.Do, so no response was
// received) is a connection error worth one retry: not the deadline or a
// cancellation, and not a network timeout.
func isConnError(ctx context.Context, err error) bool {
	if ctx.Err() != nil || errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) {
		return false
	}
	var ne net.Error
	if errors.As(err, &ne) && ne.Timeout() {
		return false
	}
	return true
}

// decodeStrict decodes exactly one JSON value into v, rejecting unknown
// fields and trailing data.
func decodeStrict(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return err
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return errors.New("trailing data after JSON value")
	}
	return nil
}

func (c *httpClient) Recommend(ctx context.Context, req RecommendRequest) (RecommendResponse, Meta, error) {
	var out RecommendResponse
	meta, err := c.call(ctx, http.MethodPost, "/v1/recommend", nil, req, &out)
	return out, meta, err
}

func (c *httpClient) Compare(ctx context.Context, req CompareRequest) (CompareResponse, Meta, error) {
	var out CompareResponse
	meta, err := c.call(ctx, http.MethodPost, "/v1/compare", nil, req, &out)
	return out, meta, err
}

func (c *httpClient) Similar(ctx context.Context, movieID int64, model ModelName, k int) (SimilarResponse, Meta, error) {
	q := url.Values{}
	if model != "" {
		q.Set("model", string(model))
	}
	if k != 0 {
		q.Set("k", strconv.Itoa(k))
	}
	var out SimilarResponse
	meta, err := c.call(ctx, http.MethodGet, "/v1/similar/"+strconv.FormatInt(movieID, 10), q, nil, &out)
	return out, meta, err
}

func (c *httpClient) Onboarding(ctx context.Context, req OnboardingRequest) (IDList, Meta, error) {
	var out IDList
	meta, err := c.call(ctx, http.MethodPost, "/v1/onboarding", nil, req, &out)
	return out, meta, err
}

func (c *httpClient) Search(ctx context.Context, q string, limit int) (IDList, Meta, error) {
	vals := url.Values{"q": {q}}
	if limit != 0 {
		vals.Set("limit", strconv.Itoa(limit))
	}
	var out IDList
	meta, err := c.call(ctx, http.MethodGet, "/v1/movies/search", vals, nil, &out)
	return out, meta, err
}

func (c *httpClient) MoviesBatch(ctx context.Context, ids []int64) (BatchResponse, Meta, error) {
	var out BatchResponse
	meta, err := c.call(ctx, http.MethodPost, "/v1/movies/batch", nil, BatchRequest{IDs: ids}, &out)
	return out, meta, err
}

func (c *httpClient) Version(ctx context.Context) (VersionResponse, Meta, error) {
	var out VersionResponse
	meta, err := c.call(ctx, http.MethodGet, "/v1/version", nil, nil, &out)
	return out, meta, err
}

func (c *httpClient) Models(ctx context.Context) (ModelsResponse, Meta, error) {
	var out ModelsResponse
	meta, err := c.call(ctx, http.MethodGet, "/v1/models", nil, nil, &out)
	return out, meta, err
}

// Healthz returns nil on 200. The body is not decoded: only the status
// matters ("200 only when a verified artifact is loaded").
func (c *httpClient) Healthz(ctx context.Context) error {
	_, err := c.call(ctx, http.MethodGet, "/healthz", nil, nil, nil)
	return err
}
