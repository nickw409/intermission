package recommend

import (
	"container/list"
	"sync"

	"github.com/nickw409/intermission/api/internal/modelclient"
)

// The recommendation cache (spec: "Recommendation cache").
//
// A cached answer is only correct for the exact inputs that produced it, so
// the key holds every one of them:
//
//   - userID and ratingsVersion: the user's ratings. Every rating write bumps
//     users.ratings_version in the same transaction, so new ratings mean a
//     new key and the old entry simply ages out.
//   - userCreated: users.created_at, which never changes for an account.
//     The real guards against one account being served another's answer
//     are that IDs are never reused (migration 0003) and that deleting an
//     account retires its entries (retireUser) (security audit SA1-1). This
//     is only a cheap extra check: created_at has one-second resolution, so
//     two accounts created in the same second share it.
//   - modelVersion: the artifact that computed the answer, taken from the
//     X-Model-Version header of the recommend/compare response itself. Before
//     every lookup the handler asks recs-model for its current version
//     (GET /v1/version) and looks up that version, so after a hot swap the
//     old answers are never found again.
//   - op, models and k: what was asked. For compare the model list is
//     sorted, so permutations share one entry; the handler renders results
//     in the order each request asked for.
//   - genres: the genre filter as a sorted, comma-joined set ("" = none),
//     so a filtered request never gets an unfiltered list or one filtered
//     by another set, while the same set in another order shares an entry.
//
// Eviction is least-recently-used under three bounds: a number of entries,
// an approximate number of bytes, and a number of entries per user (so one
// user cycling through k values can't push everyone else out).

// cacheKey identifies one cached answer.
type cacheKey struct {
	userID         int64
	userCreated    int64 // users.created_at: tells two accounts with one ID apart
	ratingsVersion int64
	modelVersion   string
	op             string // "recommend" or "compare"
	models         string // comma-joined model names (sorted for compare)
	k              int
	genres         string // sorted, comma-joined genre filter; "" = none
}

// entry is a fully enriched answer, ready to render as either response.
type entry struct {
	modelVersion string
	coldStart    bool
	results      []result
}

// result is one model's list.
type result struct {
	Model modelclient.ModelName `json:"model"`
	Items []RecItem             `json:"items"`
}

// size approximates the memory an entry and its key hold: a fixed cost per
// struct plus the bytes of every string.
func (e *entry) size(k cacheKey) int64 {
	const (
		entryOverhead   = 160
		itemOverhead    = 96
		becauseOverhead = 48
	)
	n := int64(entryOverhead + len(k.modelVersion) + len(k.models) + len(k.genres) + len(k.op) + len(e.modelVersion))
	for _, r := range e.results {
		n += int64(48 + len(r.Model))
		for _, it := range r.Items {
			n += int64(itemOverhead + len(it.Title))
			for _, b := range it.Because {
				n += int64(becauseOverhead + len(b.Title))
			}
		}
	}
	return n
}

// lru is a bounded least-recently-used map. Safe for concurrent use.
// Entries are never modified after put, so callers may share them.
type lru struct {
	mu         sync.Mutex
	maxEntries int
	maxBytes   int64
	perUser    int
	total      int64
	order      *list.List // front = most recently used; values are *lruItem
	items      map[cacheKey]*list.Element
	users      map[int64]int // entries held per user
	retired    map[int64]struct{}
}

type lruItem struct {
	key  cacheKey
	val  *entry
	size int64
}

func newLRU(maxEntries int, maxBytes int64, perUser int) *lru {
	return &lru{
		maxEntries: max(maxEntries, 1),
		maxBytes:   max(maxBytes, 1),
		perUser:    max(perUser, 1),
		order:      list.New(),
		items:      make(map[cacheKey]*list.Element),
		users:      make(map[int64]int),
		retired:    make(map[int64]struct{}),
	}
}

func (c *lru) get(k cacheKey) (*entry, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	el, ok := c.items[k]
	if !ok {
		return nil, false
	}
	c.order.MoveToFront(el)
	return el.Value.(*lruItem).val, true
}

func (c *lru) put(k cacheKey, v *entry) {
	size := v.size(k)
	c.mu.Lock()
	defer c.mu.Unlock()
	if size > c.maxBytes {
		return
	}
	if _, gone := c.retired[k.userID]; gone {
		return
	}
	if el, ok := c.items[k]; ok {
		c.removeLocked(el)
	}
	c.items[k] = c.order.PushFront(&lruItem{key: k, val: v, size: size})
	c.total += size
	c.users[k.userID]++

	// This user's least recently used entries first, then everyone's.
	if c.users[k.userID] > c.perUser {
		for el := c.order.Back(); el != nil && c.users[k.userID] > c.perUser; {
			prev := el.Prev()
			if el.Value.(*lruItem).key.userID == k.userID {
				c.removeLocked(el)
			}
			el = prev
		}
	}
	for c.order.Len() > c.maxEntries || c.total > c.maxBytes {
		c.removeLocked(c.order.Back())
	}
}

// retireUser drops every entry of userID and refuses any later put for it
// (account deletion), so a request already in flight when the account went
// can't put its answer back. IDs are never reused (migration 0003); each
// retired ID costs one map entry for the life of the process.
func (c *lru) retireUser(userID int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.retired[userID] = struct{}{}
	if c.users[userID] == 0 {
		return
	}
	for el := c.order.Front(); el != nil; {
		next := el.Next()
		if el.Value.(*lruItem).key.userID == userID {
			c.removeLocked(el)
		}
		el = next
	}
}

func (c *lru) removeLocked(el *list.Element) {
	it := el.Value.(*lruItem)
	c.order.Remove(el)
	delete(c.items, it.key)
	c.total -= it.size
	if c.users[it.key.userID]--; c.users[it.key.userID] <= 0 {
		delete(c.users, it.key.userID)
	}
}

func (c *lru) len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.order.Len()
}

func (c *lru) bytes() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.total
}
