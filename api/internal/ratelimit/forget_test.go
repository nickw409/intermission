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
