package auth

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// Password hashing with Argon2id (RFC 9106), via golang.org/x/crypto/argon2.
//
// Argon2id is a memory-hard password hash: each guess costs an attacker
// MemoryKiB of RAM for Time passes, which makes GPU/ASIC cracking of a stolen
// database expensive. A fresh random salt per password means equal passwords
// get different hashes and precomputed tables are useless.
//
// The stored form is the standard PHC string
//
//	$argon2id$v=19$m=65536,t=3,p=4$<salt, base64>$<key, base64>
//
// so the parameters travel with the hash and can be raised later without
// breaking old hashes. Keys are compared with subtle.ConstantTimeCompare.
// Each hash holds 64 MiB for its duration, so a semaphore caps concurrent
// hashes at MaxConcurrentHashes (a login flood queues instead of exhausting
// memory).

// Params are Argon2id cost parameters.
type Params struct {
	Time      uint32 // passes over memory (t)
	MemoryKiB uint32 // memory in KiB (m)
	Threads   uint8  // lanes (p)
	SaltLen   int
	KeyLen    uint32
}

// DefaultParams are the spec's parameters: t=3, m=64 MiB, p=4, 16-byte salt,
// 32-byte key.
var DefaultParams = Params{Time: 3, MemoryKiB: 64 * 1024, Threads: 4, SaltLen: 16, KeyLen: 32}

// MaxConcurrentHashes caps simultaneous Argon2id computations (spec: 4).
const MaxConcurrentHashes = 4

// Bounds accepted when parsing a stored hash, so a corrupted or planted row
// can't make one Verify allocate gigabytes or spin for minutes.
const (
	maxMemoryKiB = 256 * 1024
	maxTime      = 10
	maxThreads   = 16
	minSaltLen   = 8
	maxSaltLen   = 64
	minKeyLen    = 16
	maxKeyLen    = 64
)

// Hasher hashes and verifies passwords. The HTTP handlers depend on this
// interface so tests can count calls (both login failure paths must run
// exactly one Verify).
type Hasher interface {
	Hash(ctx context.Context, password string) (string, error)
	Verify(ctx context.Context, password, phc string) (bool, error)
}

// Argon2id is the production Hasher.
type Argon2id struct {
	params Params
	sem    chan struct{}
}

// NewArgon2id returns a Hasher running at most concurrency hashes at once.
func NewArgon2id(p Params, concurrency int) *Argon2id {
	return &Argon2id{params: p, sem: make(chan struct{}, concurrency)}
}

var errBadHash = errors.New("auth: malformed password hash")

var b64 = base64.RawStdEncoding

func (h *Argon2id) acquire(ctx context.Context) error {
	select {
	case h.sem <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (h *Argon2id) release() { <-h.sem }

// Hash returns the PHC string for password with a fresh random salt.
func (h *Argon2id) Hash(ctx context.Context, password string) (string, error) {
	p := h.params
	salt := make([]byte, p.SaltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	if err := h.acquire(ctx); err != nil {
		return "", err
	}
	key := argon2.IDKey([]byte(password), salt, p.Time, p.MemoryKiB, p.Threads, p.KeyLen)
	h.release()
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, p.MemoryKiB, p.Time, p.Threads, b64.EncodeToString(salt), b64.EncodeToString(key)), nil
}

// Verify reports whether password matches the PHC string phc. A malformed
// phc is an error (and never a match).
func (h *Argon2id) Verify(ctx context.Context, password, phc string) (bool, error) {
	p, salt, want, err := parsePHC(phc)
	if err != nil {
		return false, err
	}
	if err := h.acquire(ctx); err != nil {
		return false, err
	}
	got := argon2.IDKey([]byte(password), salt, p.Time, p.MemoryKiB, p.Threads, p.KeyLen)
	h.release()
	return subtle.ConstantTimeCompare(got, want) == 1, nil
}

// parsePHC parses $argon2id$v=19$m=M,t=T,p=P$salt$key.
func parsePHC(phc string) (Params, []byte, []byte, error) {
	parts := strings.Split(phc, "$")
	if len(parts) != 6 || parts[0] != "" || parts[1] != "argon2id" {
		return Params{}, nil, nil, errBadHash
	}
	var version int
	if _, err := fmt.Sscanf(parts[2], "v=%d", &version); err != nil || version != argon2.Version {
		return Params{}, nil, nil, errBadHash
	}
	var m, t, p uint32
	if n, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &m, &t, &p); err != nil || n != 3 ||
		fmt.Sprintf("m=%d,t=%d,p=%d", m, t, p) != parts[3] {
		return Params{}, nil, nil, errBadHash
	}
	if m < 8*p || m > maxMemoryKiB || t < 1 || t > maxTime || p < 1 || p > maxThreads {
		return Params{}, nil, nil, errBadHash
	}
	salt, err := b64.Strict().DecodeString(parts[4])
	if err != nil || len(salt) < minSaltLen || len(salt) > maxSaltLen {
		return Params{}, nil, nil, errBadHash
	}
	key, err := b64.Strict().DecodeString(parts[5])
	if err != nil || len(key) < minKeyLen || len(key) > maxKeyLen {
		return Params{}, nil, nil, errBadHash
	}
	return Params{Time: t, MemoryKiB: m, Threads: uint8(p), SaltLen: len(salt), KeyLen: uint32(len(key))}, salt, key, nil
}
