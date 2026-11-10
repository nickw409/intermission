package modelclient

import (
	"context"
	"log/slog"
	"time"
)

// DefaultTimeout is the per-call deadline recs-api applies to recs-model calls.
const DefaultTimeout = 500 * time.Millisecond

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
// TODO(workstream J): implement. Until then every call returns ErrUnavailable,
// which the public API turns into 503 model_unavailable.
func NewHTTP(cfg HTTPConfig) Client {
	return &httpClient{cfg: cfg}
}

type httpClient struct {
	cfg HTTPConfig
}

var _ Client = (*httpClient)(nil)

func (c *httpClient) Recommend(context.Context, RecommendRequest) (RecommendResponse, Meta, error) {
	return RecommendResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Compare(context.Context, CompareRequest) (CompareResponse, Meta, error) {
	return CompareResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Similar(context.Context, int64, ModelName, int) (SimilarResponse, Meta, error) {
	return SimilarResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Onboarding(context.Context, OnboardingRequest) (IDList, Meta, error) {
	return IDList{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Search(context.Context, string, int) (IDList, Meta, error) {
	return IDList{}, Meta{}, ErrUnavailable
}

func (c *httpClient) MoviesBatch(context.Context, []int64) (BatchResponse, Meta, error) {
	return BatchResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Version(context.Context) (VersionResponse, Meta, error) {
	return VersionResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Models(context.Context) (ModelsResponse, Meta, error) {
	return ModelsResponse{}, Meta{}, ErrUnavailable
}

func (c *httpClient) Healthz(context.Context) error {
	return ErrUnavailable
}
