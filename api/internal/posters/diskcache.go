package posters

import (
	"container/list"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// diskCache stores one file per movie as <dir>/<movieId><ext> (poster JPEGs,
// or movie details as JSON) with a cap on their total size, evicting the
// least recently used. File names come only from the numeric movie ID
// (never from TMDB or the request path).
//
// The index (sizes and recency) lives in memory and is rebuilt from the
// directory at startup, using modification times as the recency order;
// every hit refreshes the file's modification time so the order survives a
// restart. Writes go to a temporary file that is renamed into place, so a
// reader never sees a partial poster.
//
// File I/O happens outside the mutex; only index updates hold it. After any
// I/O the index is checked again before it is changed, because another
// request may have evicted or replaced the entry meanwhile. The races that
// remain can only make the index claim a file that is gone (an evicted
// file removed just after a new copy was renamed into place); get notices
// the missing file and drops the entry, so the disk never holds more than
// the index accounts for once writes finish.
type diskCache struct {
	dir    string
	max    int64
	kind   fileKind
	name   *regexp.Regexp // what a cache file's name looks like
	logger *slog.Logger
	broken bool // the directory can't be used; caching is off (set once)

	mu      sync.Mutex
	total   int64
	order   *list.List // front = most recently used; values are *diskItem
	entries map[int64]*list.Element
}

type diskItem struct {
	id   int64
	size int64
}

// fileKind is what one cache holds: the file extension, the largest file
// it will read back, and a check that a file read back is intact.
type fileKind struct {
	ext     string
	maxFile int64
	valid   func([]byte) bool
}

// posterFiles are poster JPEGs.
var posterFiles = fileKind{ext: ".jpg", maxFile: MaxImageBytes, valid: isJPEG}

const tmpPrefix = ".tmp-"

// newDiskCache returns a poster cache.
func newDiskCache(dir string, max int64, logger *slog.Logger) *diskCache {
	return newFileCache(dir, max, posterFiles, logger)
}

func newFileCache(dir string, max int64, kind fileKind, logger *slog.Logger) *diskCache {
	if logger == nil {
		logger = slog.Default()
	}
	c := &diskCache{dir: dir, max: max, kind: kind, logger: logger, order: list.New(), entries: map[int64]*list.Element{},
		name: regexp.MustCompile(`^[1-9][0-9]{0,9}` + regexp.QuoteMeta(kind.ext) + `$`)}
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
		if !c.name.MatchString(name) || !e.Type().IsRegular() {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		id, _ := strconv.ParseInt(strings.TrimSuffix(name, kind.ext), 10, 64)
		files = append(files, found{id: id, size: info.Size(), mtime: info.ModTime()})
	}
	// Oldest first, each pushed to the front: the newest ends up in front.
	slices.SortFunc(files, func(a, b found) int { return a.mtime.Compare(b.mtime) })
	for _, f := range files {
		c.entries[f.id] = c.order.PushFront(&diskItem{id: f.id, size: f.size})
		c.total += f.size
	}
	c.mu.Lock()
	evicted := c.evictLocked()
	c.mu.Unlock()
	c.removeFiles(evicted)
	return c
}

func (c *diskCache) path(id int64) string {
	return filepath.Join(c.dir, strconv.FormatInt(id, 10)+c.kind.ext)
}

// get returns the cached file for id.
func (c *diskCache) get(id int64) ([]byte, bool) {
	c.mu.Lock()
	el, ok := c.entries[id]
	c.mu.Unlock()
	if !ok {
		return nil, false
	}

	b, err := readCapped(c.path(id), c.kind.maxFile)

	c.mu.Lock()
	current := c.entries[id] == el
	if err != nil || !c.kind.valid(b) {
		if current {
			c.unindexLocked(el)
		}
		c.mu.Unlock()
		if current {
			c.removeFiles([]int64{id})
		}
		return nil, false
	}
	if current {
		c.order.MoveToFront(el)
	}
	c.mu.Unlock()
	now := time.Now()
	_ = os.Chtimes(c.path(id), now, now)
	return b, true
}

// put stores a file and evicts until the total fits the cap. A file bigger
// than the whole cap, or than get would read back, is not stored.
func (c *diskCache) put(id int64, data []byte) {
	size := int64(len(data))
	if c.broken || size > c.max || size > c.kind.maxFile {
		return
	}
	if err := c.write(id, data); err != nil {
		c.logger.Warn("posters: cache write failed", "err", err)
		return
	}
	c.mu.Lock()
	if el, ok := c.entries[id]; ok {
		item := el.Value.(*diskItem)
		c.total += size - item.size
		item.size = size
		c.order.MoveToFront(el)
	} else {
		c.entries[id] = c.order.PushFront(&diskItem{id: id, size: size})
		c.total += size
	}
	evicted := c.evictLocked()
	c.mu.Unlock()
	c.removeFiles(evicted)
}

// write puts data at id's path through a temporary file and a rename.
func (c *diskCache) write(id int64, data []byte) error {
	f, err := os.CreateTemp(c.dir, tmpPrefix+"*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	_, werr := f.Write(data)
	cerr := f.Close()
	if err := errors.Join(werr, cerr); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, c.path(id)); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// evictLocked drops least recently used entries from the index until
// total <= max and returns their IDs; the caller removes the files after
// unlocking.
func (c *diskCache) evictLocked() []int64 {
	var ids []int64
	for c.total > c.max && c.order.Len() > 0 {
		el := c.order.Back()
		ids = append(ids, el.Value.(*diskItem).id)
		c.unindexLocked(el)
	}
	return ids
}

func (c *diskCache) unindexLocked(el *list.Element) {
	item := el.Value.(*diskItem)
	c.order.Remove(el)
	delete(c.entries, item.id)
	c.total -= item.size
}

func (c *diskCache) removeFiles(ids []int64) {
	for _, id := range ids {
		if err := os.Remove(c.path(id)); err != nil && !os.IsNotExist(err) {
			c.logger.Warn("posters: cache eviction failed", "err", err)
		}
	}
}

func (c *diskCache) bytes() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.total
}

// readCapped reads a cache file with openRegular, refusing files over limit
// bytes.
func readCapped(path string, limit int64) ([]byte, error) {
	f, err := openRegular(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("cached file too large")
	}
	return b, nil
}

// openRegular opens path for reading only if its last component is not a
// symlink (O_NOFOLLOW) and the opened file is a regular file (checked on the
// open descriptor). O_NONBLOCK keeps a FIFO from blocking the open; it is
// then refused as not regular.
func openRegular(path string) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	if !info.Mode().IsRegular() {
		f.Close()
		return nil, errors.New("not a regular file")
	}
	return f, nil
}
