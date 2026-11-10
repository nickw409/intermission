package reqid

import (
	"context"
	"strings"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	ctx := WithID(context.Background(), "abc")
	if got := FromContext(ctx); got != "abc" {
		t.Fatalf("FromContext = %q, want abc", got)
	}
	if got := FromContext(context.Background()); got != "" {
		t.Fatalf("FromContext(empty) = %q, want \"\"", got)
	}
}

func TestNew(t *testing.T) {
	a, b := New(), New()
	if len(a) != 32 || !Valid(a) {
		t.Fatalf("New() = %q, want 32 valid hex chars", a)
	}
	if a == b {
		t.Fatalf("New() returned the same ID twice: %q", a)
	}
}

func TestValid(t *testing.T) {
	cases := map[string]bool{
		"":                            false,
		"abc-DEF_1.2":                 true,
		strings.Repeat("a", MaxLen):   true,
		strings.Repeat("a", MaxLen+1): false,
		"has space":                   false,
		"new\nline":                   false,
		"quote\"":                     false,
	}
	for in, want := range cases {
		if got := Valid(in); got != want {
			t.Errorf("Valid(%q) = %v, want %v", in, got, want)
		}
	}
}
