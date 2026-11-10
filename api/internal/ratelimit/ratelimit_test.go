package ratelimit

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync"
	"testing"
	"time"
)

type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *fakeClock) Now() time.Time { c.mu.Lock(); defer c.mu.Unlock(); return c.t }
func (c *fakeClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.t = c.t.Add(d)
}

func newClock() *fakeClock { return &fakeClock{t: time.Unix(1_800_000_000, 0)} }

func TestLoginRateFivePerMinutePerKey(t *testing.T) {
	clk := newClock()
	l := NewKeyed(LoginPerAccount, WithClock(clk.Now))
	for i := 0; i < 5; i++ {
		if ok, _ := l.Allow("alice"); !ok {
			t.Fatalf("attempt %d denied", i+1)
		}
	}
	ok, retry := l.Allow("alice")
	if ok {
		t.Fatal("6th attempt within a minute allowed")
	}
	if retry <= 0 || retry > 12*time.Second {
		t.Fatalf("retry = %v, want (0, 12s]", retry)
	}
	// Other keys are independent.
	if ok, _ := l.Allow("bob"); !ok {
		t.Fatal("bob limited by alice's bucket")
	}
	// A denied attempt does not consume a token: after the retry delay exactly
	// one more attempt is allowed.
	clk.Advance(retry)
	if ok, _ := l.Allow("alice"); !ok {
		t.Fatal("not allowed after Retry-After")
	}
	if ok, _ := l.Allow("alice"); ok {
		t.Fatal("two allowed after one refill")
	}
	// Steady state: 5 per minute.
	clk.Advance(time.Minute)
	n := 0
	for i := 0; i < 20; i++ {
		if ok, _ := l.Allow("alice"); ok {
			n++
		}
	}
	if n != 5 {
		t.Fatalf("allowed %d after a minute, want 5", n)
	}
}

func TestGlobalCap(t *testing.T) {
	clk := newClock()
	g := NewGlobal(Config{Every: time.Second, Burst: 3}, WithClock(clk.Now))
	for i := 0; i < 3; i++ {
		if ok, _ := g.Allow(); !ok {
			t.Fatalf("attempt %d denied", i)
		}
	}
	if ok, retry := g.Allow(); ok || retry != time.Second {
		t.Fatalf("ok=%v retry=%v", ok, retry)
	}
}

func TestKeyedEvictsIdleBucketsAndStaysBounded(t *testing.T) {
	clk := newClock()
	l := NewKeyed(Config{Every: time.Second, Burst: 2}, WithClock(clk.Now), WithMaxKeys(10))
	for i := 0; i < 10; i++ {
		l.Allow("k" + strconv.Itoa(i))
	}
	// Full and nothing is idle: a new key fails closed rather than evicting a
	// bucket that still remembers recent attempts.
	if ok, _ := l.Allow("new"); ok {
		t.Fatal("new key admitted while every bucket is still live")
	}
	// Once buckets have refilled they carry no state and may be dropped.
	clk.Advance(3 * time.Second)
	if ok, _ := l.Allow("new"); !ok {
		t.Fatal("new key denied after idle buckets expired")
	}
	if n := l.Len(); n > 10 {
		t.Fatalf("len = %d", n)
	}
}

func TestMiddlewareWrites429WithRetryAfter(t *testing.T) {
	clk := newClock()
	l := NewKeyed(Config{Every: 30 * time.Second, Burst: 1}, WithClock(clk.Now))
	h := Middleware(l, func(r *http.Request) (string, bool) { return r.Header.Get("X-Key"), true })(
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusNoContent) }))

	do := func() *httptest.ResponseRecorder {
		req := httptest.NewRequest("GET", "/api/recommend", nil)
		req.Header.Set("X-Key", "s1")
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec
	}
	if rec := do(); rec.Code != http.StatusNoContent {
		t.Fatalf("first = %d", rec.Code)
	}
	rec := do()
	if rec.Code != http.StatusTooManyRequests || rec.Header().Get("Retry-After") != "30" {
		t.Fatalf("second = %d, Retry-After %q", rec.Code, rec.Header().Get("Retry-After"))
	}
	var body struct {
		Error struct{ Code, Message string }
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil || body.Error.Code != "rate_limited" || body.Error.Message != "too many requests" {
		t.Fatalf("body = %s", rec.Body.String())
	}
}

func TestRetryAfterSecondsRoundsUp(t *testing.T) {
	for d, want := range map[time.Duration]string{
		time.Millisecond: "1", time.Second: "1", 1500 * time.Millisecond: "2", 12 * time.Second: "12", 0: "1",
	} {
		if got := RetryAfterSeconds(d); got != want {
			t.Errorf("%v -> %s, want %s", d, got, want)
		}
	}
}

func TestConcurrentAllowIsExact(t *testing.T) {
	l := NewKeyed(Config{Every: time.Hour, Burst: 7})
	var wg sync.WaitGroup
	var mu sync.Mutex
	allowed := 0
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if ok, _ := l.Allow("same"); ok {
				mu.Lock()
				allowed++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if allowed != 7 {
		t.Fatalf("allowed %d, want 7", allowed)
	}
}
