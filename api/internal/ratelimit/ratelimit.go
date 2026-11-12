package ratelimit

import (
	"math"
	"net/http"
	"strconv"
	"sync"
	"time"

	"golang.org/x/time/rate"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// Token bucket, in one paragraph: a bucket holds up to Burst tokens and gains
// one token every Every. Each request takes a token; with none left it is
// refused, and the caller is told how long until the next token arrives
// (Retry-After). The buckets themselves are golang.org/x/time/rate.Limiter.

// Config is one bucket shape: one token every Every, at most Burst stored.
type Config struct {
	Every time.Duration
	Burst int
}

// Limits from the spec ("Rate limits") plus the global caps.
var (
	// LoginPerAccount: 5 login attempts per minute per case-folded username,
	// whether or not the account exists.
	LoginPerAccount = Config{Every: time.Minute / 5, Burst: 5}
	// RecommendPerSession: 30 /api/recommend calls per minute per session.
	RecommendPerSession = Config{Every: time.Minute / 30, Burst: 30}
	// FeedbackPerSession: 60 POST /api/feedback calls per minute per session.
	FeedbackPerSession = Config{Every: time.Minute / 60, Burst: 60}
	// LoginGlobal caps login attempts across all accounts (each costs an
	// Argon2id hash): 60 per minute.
	LoginGlobal = Config{Every: time.Second, Burst: 30}
	// SignupGlobal caps signup attempts across all callers: 10 per minute.
	SignupGlobal = Config{Every: 6 * time.Second, Burst: 10}
)

// DefaultMaxKeys bounds the number of buckets a Keyed limiter holds.
const DefaultMaxKeys = 10_000

// Option configures a limiter.
type Option func(*options)

type options struct {
	now     func() time.Time
	maxKeys int
}

// WithClock replaces time.Now (tests).
func WithClock(now func() time.Time) Option { return func(o *options) { o.now = now } }

// WithMaxKeys bounds how many keys a Keyed limiter tracks.
func WithMaxKeys(n int) Option { return func(o *options) { o.maxKeys = n } }

func buildOptions(opts []Option) options {
	o := options{now: time.Now, maxKeys: DefaultMaxKeys}
	for _, f := range opts {
		f(&o)
	}
	return o
}

// Keyed holds one token bucket per key (account, session, ...).
type Keyed struct {
	cfg  Config
	opts options

	mu      sync.Mutex
	buckets map[string]*bucket
	retired map[string]struct{} // see Retire
}

type bucket struct {
	lim  *rate.Limiter
	last time.Time
}

// NewKeyed returns a per-key limiter.
func NewKeyed(cfg Config, opts ...Option) *Keyed {
	return &Keyed{cfg: cfg, opts: buildOptions(opts), buckets: make(map[string]*bucket)}
}

// refillTime is how long an untouched bucket takes to become full again;
// after that it is indistinguishable from a new one and can be dropped.
func (k *Keyed) refillTime() time.Duration { return k.cfg.Every * time.Duration(k.cfg.Burst) }

// Allow takes a token for key. It returns false and the wait until the next
// token when the bucket is empty. A refused call takes no token.
func (k *Keyed) Allow(key string) (bool, time.Duration) {
	now := k.opts.now()
	k.mu.Lock()
	defer k.mu.Unlock()
	if _, gone := k.retired[key]; gone {
		return false, k.cfg.Every
	}
	b, ok := k.buckets[key]
	if !ok {
		if len(k.buckets) >= k.opts.maxKeys {
			k.sweep(now)
		}
		if len(k.buckets) >= k.opts.maxKeys {
			// Every tracked bucket is still live. Fail closed: dropping a live
			// bucket would let a flood of fresh keys reset an attacked account.
			return false, k.cfg.Every
		}
		b = &bucket{lim: rate.NewLimiter(rate.Every(k.cfg.Every), k.cfg.Burst)}
		k.buckets[key] = b
	}
	b.last = now
	return take(b.lim, now)
}

// sweep drops buckets that have been idle long enough to be full again.
func (k *Keyed) sweep(now time.Time) {
	idle := k.refillTime()
	for key, b := range k.buckets {
		if now.Sub(b.last) >= idle {
			delete(k.buckets, key)
		}
	}
}

// Forget drops key's bucket, e.g. when the account it belongs to is
// deleted. Unknown keys are ignored.
func (k *Keyed) Forget(key string) {
	k.mu.Lock()
	defer k.mu.Unlock()
	delete(k.buckets, key)
}

// Retire drops key's bucket for good: every later Allow(key) is refused and
// creates no bucket. For keys that can never be legitimately used again (a
// deleted account's ID; IDs are never reused), so a request already in
// flight when the account went can't bring the bucket back. Each retired
// key costs one map entry for the life of the process.
func (k *Keyed) Retire(key string) {
	k.mu.Lock()
	defer k.mu.Unlock()
	delete(k.buckets, key)
	if k.retired == nil {
		k.retired = make(map[string]struct{})
	}
	k.retired[key] = struct{}{}
}

// Len returns the number of tracked keys.
func (k *Keyed) Len() int {
	k.mu.Lock()
	defer k.mu.Unlock()
	return len(k.buckets)
}

func take(lim *rate.Limiter, now time.Time) (bool, time.Duration) {
	r := lim.ReserveN(now, 1)
	if !r.OK() {
		return false, time.Minute
	}
	if d := r.DelayFrom(now); d > 0 {
		r.CancelAt(now)
		return false, d
	}
	return true, 0
}

// Global is a single shared bucket.
type Global struct {
	lim *rate.Limiter
	now func() time.Time
	mu  sync.Mutex
}

// NewGlobal returns a global limiter.
func NewGlobal(cfg Config, opts ...Option) *Global {
	o := buildOptions(opts)
	return &Global{lim: rate.NewLimiter(rate.Every(cfg.Every), cfg.Burst), now: o.now}
}

// Allow takes a token; see Keyed.Allow.
func (g *Global) Allow() (bool, time.Duration) {
	g.mu.Lock()
	defer g.mu.Unlock()
	return take(g.lim, g.now())
}

// RetryAfterSeconds formats d for the Retry-After header: whole seconds,
// rounded up, at least 1.
func RetryAfterSeconds(d time.Duration) string {
	s := int64(math.Ceil(d.Seconds()))
	if s < 1 {
		s = 1
	}
	return strconv.FormatInt(s, 10)
}

// WriteLimited writes 429 rate_limited with Retry-After.
func WriteLimited(w http.ResponseWriter, retry time.Duration) {
	w.Header().Set("Retry-After", RetryAfterSeconds(retry))
	apperr.WriteError(w, http.StatusTooManyRequests, apperr.CodeRateLimited, "too many requests")
}

// Middleware limits requests per key. key returns the bucket key for a
// request; ok=false skips limiting (e.g. no session: the handler will 401).
//
// Workstream J wires it on /api/recommend with RecommendPerSession and
// auth.SessionKey, after auth's session middleware.
func Middleware(l *Keyed, key func(*http.Request) (string, bool)) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if k, ok := key(r); ok {
				if allowed, retry := l.Allow(k); !allowed {
					WriteLimited(w, retry)
					return
				}
			}
			next.ServeHTTP(w, r)
		})
	}
}
