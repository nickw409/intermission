package posters

import (
	"container/list"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

// diskCache stores poster JPEGs as <dir>/<movieId>.jpg with a cap on their
// total size, evicting the least recently used. File names come only from
// the numeric movie ID (never from TMDB or the request path).
//
// The index (sizes and recency) lives in memory and is rebuilt from the
// directory at startup, using modification times as the recency order;
// every hit refreshes the file's modification time so the order survives a
// restart. Writes go to a temporary file that is renamed into place, so a
// reader never sees a partial poster.
type diskCache struct {
	dir    string
	max    int64
	logger *slog.Logger

	mu      sync.Mutex
	total   int64
	order   *list.List // front = most recently used; values are *diskItem
	entries map[int64]*list.Element
	broken  bool // the directory can't be used; caching is off
}

type diskItem struct {
	id   int64
	size int64
}

var cacheFileName = regexp.MustCompile(`^[1-9][0-9]{0,9}\.jpg$`)

const tmpPrefix = ".tmp-"

func newDiskCache(dir string, max int64, logger *slog.Logger) *diskCache {
	if logger == nil {
		logger = slog.Default()
	}
	c := &diskCache{dir: dir, max: max, logger: logger, order: list.New(), entries: map[int64]*list.Element{}}
	if err := os.MkdirAll(dir, 0o750); err != nil {
		logger.Warn("posters: disk cache disabled", "err", err)
		c.broken = true
		return c
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		logger.Warn("posters: disk cache disabled", "err", err)
		c.broken = true
		return c
	}
	type found struct {
		id    int64
		size  int64
		mtime time.Time
	}
	var files []found
	for _, e := range ents {
		name := e.Name()
		if strings.HasPrefix(name, tmpPrefix) && e.Type().IsRegular() {
			_ = os.Remove(filepath.Join(dir, name)) // left by a crash mid-write
			continue
		}
		if !cacheFileName.MatchString(name) || !e.Type().IsRegular() {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		id, _ := strconv.ParseInt(strings.TrimSuffix(name, ".jpg"), 10, 64)
		files = append(files, found{id: id, size: info.Size(), mtime: info.ModTime()})
	}
	// Oldest first, each pushed to the front: the newest ends up in front.
	slices.SortFunc(files, func(a, b found) int { return a.mtime.Compare(b.mtime) })
	for _, f := range files {
		c.entries[f.id] = c.order.PushFront(&diskItem{id: f.id, size: f.size})
		c.total += f.size
	}
	c.mu.Lock()
	c.evictLocked()
	c.mu.Unlock()
	return c
}

func (c *diskCache) path(id int64) string {
	return filepath.Join(c.dir, strconv.FormatInt(id, 10)+".jpg")
}

// get returns the cached poster for id.
func (c *diskCache) get(id int64) ([]byte, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	el, ok := c.entries[id]
	if !ok {
		return nil, false
	}
	b, err := os.ReadFile(c.path(id))
	if err != nil || int64(len(b)) > MaxImageBytes || !isJPEG(b) {
		c.removeLocked(el)
		return nil, false
	}
	c.order.MoveToFront(el)
	now := time.Now()
	_ = os.Chtimes(c.path(id), now, now)
	return b, true
}

// put stores a poster and evicts until the total fits the cap. A poster
// bigger than the whole cap is not stored.
func (c *diskCache) put(id int64, data []byte) {
	size := int64(len(data))
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.broken || size > c.max {
		return
	}
	f, err := os.CreateTemp(c.dir, tmpPrefix+"*")
	if err != nil {
		c.logger.Warn("posters: cache write failed", "err", err)
		return
	}
	tmp := f.Name()
	_, werr := f.Write(data)
	cerr := f.Close()
	if werr != nil || cerr != nil {
		_ = os.Remove(tmp)
		c.logger.Warn("posters: cache write failed", "err", firstErr(werr, cerr))
		return
	}
	if err := os.Rename(tmp, c.path(id)); err != nil {
		_ = os.Remove(tmp)
		c.logger.Warn("posters: cache write failed", "err", err)
		return
	}
	if el, ok := c.entries[id]; ok {
		item := el.Value.(*diskItem)
		c.total += size - item.size
		item.size = size
		c.order.MoveToFront(el)
	} else {
		c.entries[id] = c.order.PushFront(&diskItem{id: id, size: size})
		c.total += size
	}
	c.evictLocked()
}

// evictLocked removes least recently used posters until total <= max.
func (c *diskCache) evictLocked() {
	for c.total > c.max && c.order.Len() > 0 {
		c.removeLocked(c.order.Back())
	}
}

func (c *diskCache) removeLocked(el *list.Element) {
	item := el.Value.(*diskItem)
	if err := os.Remove(c.path(item.id)); err != nil && !os.IsNotExist(err) {
		c.logger.Warn("posters: cache eviction failed", "err", err)
	}
	c.order.Remove(el)
	delete(c.entries, item.id)
	c.total -= item.size
}

func firstErr(errs ...error) error {
	for _, err := range errs {
		if err != nil {
			return err
		}
	}
	return nil
}
