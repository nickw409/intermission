package posters

import (
	"container/list"
	"sync"
	"time"
)

// negCache remembers, for a short time, movie IDs that have no poster to
// fetch: unknown to recs-model, no TMDB id, no TMDB poster, or a failed
// TMDB fetch. Those requests get the placeholder without another lookup.
// It holds at most max IDs; the oldest additions go first.
type negCache struct {
	mu    sync.Mutex
	ttl   time.Duration
	max   int
	now   func() time.Time
	until map[int64]*list.Element // values are *negItem
	order *list.List              // front = oldest addition
}

type negItem struct {
	id      int64
	expires time.Time
}

func newNegCache(limit int, ttl time.Duration, now func() time.Time) *negCache {
	if now == nil {
		now = time.Now
	}
	return &negCache{ttl: ttl, max: max(limit, 1), now: now, until: map[int64]*list.Element{}, order: list.New()}
}

// has reports whether id is remembered and not yet expired.
func (n *negCache) has(id int64) bool {
	n.mu.Lock()
	defer n.mu.Unlock()
	el, ok := n.until[id]
	if !ok {
		return false
	}
	if !n.now().Before(el.Value.(*negItem).expires) {
		n.order.Remove(el)
		delete(n.until, id)
		return false
	}
	return true
}

// add remembers id for ttl.
func (n *negCache) add(id int64) {
	n.mu.Lock()
	defer n.mu.Unlock()
	if el, ok := n.until[id]; ok {
		n.order.Remove(el)
	}
	n.until[id] = n.order.PushBack(&negItem{id: id, expires: n.now().Add(n.ttl)})
	for n.order.Len() > n.max {
		oldest := n.order.Front()
		n.order.Remove(oldest)
		delete(n.until, oldest.Value.(*negItem).id)
	}
}

func (n *negCache) len() int {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.order.Len()
}
