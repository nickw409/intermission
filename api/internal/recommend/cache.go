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
//   - modelVersion: the artifact that computed the answer, taken from the
//     X-Model-Version header of the recommend/compare response itself. Before
//     every lookup the handler asks recs-model for its current version
//     (GET /v1/version) and looks up that version, so after a hot swap the
//     old answers are never found again.
//   - op, models and k: what was asked.
//
// Eviction is least-recently-used with a fixed number of entries.

// cacheKey identifies one cached answer.
type cacheKey struct {
	userID         int64
	ratingsVersion int64
	modelVersion   string
	op             string // "recommend" or "compare"
	models         string // comma-joined model names, in request order
	k              int
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

// lru is a fixed-size least-recently-used map. Safe for concurrent use.
// Entries are never modified after put, so callers may share them.
type lru struct {
	mu    sync.Mutex
	max   int
	order *list.List // front = most recently used; values are *lruItem
	items map[cacheKey]*list.Element
}

type lruItem struct {
	key cacheKey
	val *entry
}

func newLRU(max int) *lru {
	if max < 1 {
		max = 1
	}
	return &lru{max: max, order: list.New(), items: make(map[cacheKey]*list.Element)}
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
	c.mu.Lock()
	defer c.mu.Unlock()
	if el, ok := c.items[k]; ok {
		el.Value.(*lruItem).val = v
		c.order.MoveToFront(el)
		return
	}
	c.items[k] = c.order.PushFront(&lruItem{key: k, val: v})
	for c.order.Len() > c.max {
		oldest := c.order.Back()
		c.order.Remove(oldest)
		delete(c.items, oldest.Value.(*lruItem).key)
	}
}

func (c *lru) len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.order.Len()
}
