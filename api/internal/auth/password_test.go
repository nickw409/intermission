package auth

import (
	"context"
	"errors"
	"regexp"
	"strings"
	"testing"
	"time"
)

// fastParams keep the many HTTP tests quick; the production parameters are
// exercised by TestArgon2idProductionParams.
var fastParams = Params{Time: 1, MemoryKiB: 64, Threads: 1, SaltLen: 16, KeyLen: 32}

func TestArgon2idProductionParams(t *testing.T) {
	if DefaultParams != (Params{Time: 3, MemoryKiB: 64 * 1024, Threads: 4, SaltLen: 16, KeyLen: 32}) {
		t.Fatalf("DefaultParams = %+v; the spec fixes t=3, m=64 MiB, p=4, 16-byte salt, 32-byte key", DefaultParams)
	}
	h := NewArgon2id(DefaultParams, MaxConcurrentHashes)
	ctx := context.Background()
	phc, err := h.Hash(ctx, "correct horse battery")
	if err != nil {
		t.Fatal(err)
	}
	// $argon2id$v=19$m=65536,t=3,p=4$<22-char salt>$<43-char key>
	re := regexp.MustCompile(`^\$argon2id\$v=19\$m=65536,t=3,p=4\$[A-Za-z0-9+/]{22}\$[A-Za-z0-9+/]{43}$`)
	if !re.MatchString(phc) {
		t.Fatalf("PHC string %q", phc)
	}
	if ok, err := h.Verify(ctx, "correct horse battery", phc); err != nil || !ok {
		t.Fatalf("verify: %v %v", ok, err)
	}
	if ok, err := h.Verify(ctx, "correct horse batterY", phc); err != nil || ok {
		t.Fatalf("wrong password verified: %v %v", ok, err)
	}
}

func TestArgon2idSaltIsRandomAndTamperingFails(t *testing.T) {
	h := NewArgon2id(fastParams, MaxConcurrentHashes)
	ctx := context.Background()
	a, _ := h.Hash(ctx, "same password")
	b, _ := h.Hash(ctx, "same password")
	if a == b {
		t.Fatal("two hashes of one password are equal: salt not random")
	}
	parts := strings.Split(a, "$")
	key := []byte(parts[5])
	if key[0] == 'A' {
		key[0] = 'B'
	} else {
		key[0] = 'A'
	}
	tampered := strings.Join(append(parts[:5:5], string(key)), "$")
	if ok, _ := h.Verify(ctx, "same password", tampered); ok {
		t.Fatal("tampered hash verified")
	}
}

func TestVerifyRejectsMalformedAndAbusiveHashes(t *testing.T) {
	h := NewArgon2id(fastParams, MaxConcurrentHashes)
	ctx := context.Background()
	for _, phc := range []string{
		"",
		"plaintext",
		"$argon2i$v=19$m=64,t=1,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=16$m=64,t=1,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=19$m=64,t=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=19$m=64,t=1,p=1$!!!$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		// Parameters far beyond what this server ever writes (DoS guard).
		"$argon2id$v=19$m=4194304,t=1,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=19$m=64,t=1000,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=19$m=64,t=1,p=255$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
		"$argon2id$v=19$m=64,t=0,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g",
	} {
		if ok, err := h.Verify(ctx, "password", phc); ok || err == nil {
			t.Errorf("Verify(%q) = %v, %v; want false and an error", phc, ok, err)
		}
	}
}

// At most MaxConcurrentHashes hashes run at once; the rest wait (and give up
// when their request is cancelled).
func TestHashSemaphoreCapsConcurrency(t *testing.T) {
	if MaxConcurrentHashes != 4 {
		t.Fatalf("MaxConcurrentHashes = %d, spec says 4", MaxConcurrentHashes)
	}
	h := NewArgon2id(fastParams, MaxConcurrentHashes)
	for i := 0; i < MaxConcurrentHashes; i++ {
		h.sem <- struct{}{} // occupy every slot
	}
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if _, err := h.Hash(ctx, "password123"); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Hash with a full semaphore: %v", err)
	}
	if _, err := h.Verify(ctx, "password123", "$argon2id$v=19$m=64,t=1,p=1$c29tZXNhbHRzb21lc2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g"); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Verify with a full semaphore: %v", err)
	}
	<-h.sem // free one slot
	if _, err := h.Hash(context.Background(), "password123"); err != nil {
		t.Fatalf("Hash with a free slot: %v", err)
	}
}
