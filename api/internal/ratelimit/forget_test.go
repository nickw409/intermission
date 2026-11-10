package ratelimit

import (
	"testing"
	"time"
)

// Forget drops one key's bucket (account deletion, security audit SA1-1):
// the key starts over with a full bucket and other keys keep theirs.
func TestKeyedForget(t *testing.T) {
	k := NewKeyed(Config{Every: time.Hour, Burst: 1})
	for _, key := range []string{"user:1", "user:2"} {
		if ok, _ := k.Allow(key); !ok {
			t.Fatalf("%s: first call refused", key)
		}
	}
	k.Forget("user:1")
	if k.Len() != 1 {
		t.Fatalf("Len = %d after Forget", k.Len())
	}
	if ok, _ := k.Allow("user:1"); !ok {
		t.Fatal("forgotten key still limited")
	}
	if ok, _ := k.Allow("user:2"); ok {
		t.Fatal("other key's bucket was reset")
	}
	k.Forget("never-seen") // no-op
}

// Retire drops the bucket for good: later calls are refused and track
// nothing; other keys are untouched.
func TestKeyedRetire(t *testing.T) {
	k := NewKeyed(Config{Every: time.Millisecond, Burst: 10})
	k.Allow("user:1")
	k.Allow("user:2")
	k.Retire("user:1")
	for i := 0; i < 3; i++ {
		if ok, retry := k.Allow("user:1"); ok || retry <= 0 {
			t.Fatalf("retired key allowed (ok=%v retry=%v)", ok, retry)
		}
	}
	if k.Len() != 1 {
		t.Fatalf("Len = %d, want 1 (only user:2)", k.Len())
	}
	if ok, _ := k.Allow("user:2"); !ok {
		t.Fatal("other key refused")
	}
}
