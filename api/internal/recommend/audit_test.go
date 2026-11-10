package recommend

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// bigEntry returns an entry whose approximate size is dominated by n bytes
// of title.
func bigEntry(n int) *entry {
	return &entry{modelVersion: "v", results: []result{{Model: "pop", Items: []RecItem{{MovieID: 1, Title: strings.Repeat("t", n), Because: []Because{}}}}}}
}

// Audit M2: the cache is bounded by approximate bytes as well as entries.
func TestLRUByteCap(t *testing.T) {
	const capBytes = 10_000
	c := newLRU(1000, capBytes, 1000)
	for i := 0; i < 50; i++ {
		c.put(cacheKey{userID: int64(i)}, bigEntry(1000))
		if c.bytes() > capBytes {
			t.Fatalf("after %d puts: %d bytes > cap %d", i+1, c.bytes(), capBytes)
		}
	}
	if n := c.len(); n == 0 || n > capBytes/1000 {
		t.Fatalf("%d entries", n)
	}
	if _, ok := c.get(cacheKey{userID: 49}); !ok {
		t.Fatal("newest entry evicted")
	}
	// An entry bigger than the whole cap is not stored and evicts nothing.
	before := c.len()
	c.put(cacheKey{userID: 999}, bigEntry(2*capBytes))
	if _, ok := c.get(cacheKey{userID: 999}); ok || c.len() != before {
		t.Fatalf("oversized entry stored (len %d -> %d)", before, c.len())
	}
	// Replacing an entry accounts for the old size.
	c2 := newLRU(10, capBytes, 10)
	for i := 0; i < 100; i++ {
		c2.put(cacheKey{userID: 1}, bigEntry(1000))
	}
	if c2.len() != 1 || c2.bytes() > 2000 {
		t.Fatalf("replace: len %d bytes %d", c2.len(), c2.bytes())
	}
}

// Audit M2: one user can't fill the cache; their least recently used
// entries go first once they hold perUser entries.
func TestLRUPerUserCap(t *testing.T) {
	c := newLRU(100, 1<<20, 3)
	c.put(cacheKey{userID: 2, k: 1}, &entry{})
	for k := 1; k <= 5; k++ {
		c.put(cacheKey{userID: 1, k: k}, &entry{})
	}
	for k := 1; k <= 5; k++ {
		if _, ok := c.get(cacheKey{userID: 1, k: k}); ok != (k >= 3) {
			t.Errorf("user 1 k=%d cached = %v", k, ok)
		}
	}
	if _, ok := c.get(cacheKey{userID: 2, k: 1}); !ok {
		t.Error("another user's entry evicted")
	}
	if c.len() != 4 {
		t.Errorf("len %d", c.len())
	}
}

// Audit M2: the compare key doesn't depend on model order; each response
// still lists results in the order the caller asked for.
func TestCompareKeyNormalized(t *testing.T) {
	m := newVersioned(t)
	e := newEnv(t, m)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	order := func(rec *httptest.ResponseRecorder) string {
		var b struct {
			Results []struct {
				Model string `json:"model"`
			} `json:"results"`
		}
		_ = json.Unmarshal(rec.Body.Bytes(), &b)
		var s []string
		for _, r := range b.Results {
			s = append(s, r.Model)
		}
		return strings.Join(s, ",")
	}
	if rec := get(e, "/api/recommend/compare?models=pop,knn,als&k=3", tok); order(rec) != "pop,knn,als" {
		t.Fatalf("first: %s", rec.Body.String())
	}
	rec := get(e, "/api/recommend/compare?models=als,pop,knn&k=3", tok)
	if order(rec) != "als,pop,knn" {
		t.Fatalf("reordered: %s", rec.Body.String())
	}
	if m.compares.Load() != 1 {
		t.Fatalf("%d compare calls; the permutation missed the cache", m.compares.Load())
	}
}

// Audit L3: the 30/min budget is also per user, so logging in again for a
// fresh session doesn't reset it.
func TestRateLimitPerUser(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok1 := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	tok2 := newSession(t, e, uid)
	for i := 0; i < 30; i++ {
		tok := tok1
		if i%2 == 1 {
			tok = tok2
		}
		if rec := get(e, "/api/recommend?model=als&k=3", tok); rec.Code != 200 {
			t.Fatalf("request %d: %d", i+1, rec.Code)
		}
	}
	tok3 := newSession(t, e, uid)
	for _, tok := range []string{tok1, tok2, tok3} {
		if rec := get(e, "/api/recommend?model=als&k=3", tok); rec.Code != 429 {
			t.Fatalf("31st request: %d", rec.Code)
		}
	}
}

// newSession adds another live session for uid, as a second login would.
// The token has the real format (43 base64url characters).
func newSession(t *testing.T, e *cachetest.Env, uid int64) string {
	t.Helper()
	raw := sha256.Sum256([]byte(fmt.Sprintf("%d-%d", uid, time.Now().UnixNano())))
	tok := base64.RawURLEncoding.EncodeToString(raw[:])
	sum := sha256.Sum256([]byte(tok))
	now := time.Now().Unix()
	if _, err := e.DB.Writer.Exec("INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
		sum[:], uid, now, now+3600); err != nil {
		t.Fatal(err)
	}
	return tok
}

// Info: model names echoed in errors are truncated.
func TestUnknownModelMessageTruncated(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	long := strings.Repeat("x", 500)
	for _, path := range []string{"/api/recommend?model=" + long, "/api/recommend/compare?models=pop," + long[:50]} {
		rec := get(e, path, tok)
		var b struct {
			Error struct{ Message string } `json:"error"`
		}
		_ = json.Unmarshal(rec.Body.Bytes(), &b)
		if rec.Code != 400 || len(b.Error.Message) > 64 {
			t.Fatalf("%d %q", rec.Code, b.Error.Message)
		}
	}
}
