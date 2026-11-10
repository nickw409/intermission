package modelclient

import (
	"context"
	"errors"
	"testing"
)

// The stub must degrade to ErrUnavailable until workstream J replaces it.
func TestStubReturnsUnavailable(t *testing.T) {
	c := NewHTTP(HTTPConfig{SocketPath: "/nonexistent.sock"})
	ctx := context.Background()
	if _, _, err := c.Recommend(ctx, RecommendRequest{}); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Recommend err = %v", err)
	}
	if err := c.Healthz(ctx); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("Healthz err = %v", err)
	}
}

func TestStatusErrorString(t *testing.T) {
	e := &StatusError{Status: 400, Code: ErrorCodeUnknownModel, Message: "no such model"}
	if got := e.Error(); got != "recs-model: 400 unknown_model: no such model" {
		t.Fatalf("Error() = %q", got)
	}
}
