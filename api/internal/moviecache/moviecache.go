// Package moviecache is recs-api's own copy of movie metadata, stored in
// SQLite (movie_id, title, year, genres, tmdb_id, rating_count, mean_rating,
// updated_at). It is filled from every /v1/movies/batch response (search,
// onboarding and recommend results are enriched through batch), and ratings
// PUT/GET read from it, so ratings keep working while recs-model is down.
//
// Freshness: rows are never invalidated, including when recs-model loads a
// new artifact. Title, year, genres and TMDB ID are stable catalog data;
// rating_count and mean_rating can lag. Refresh (used by the movie page, the
// only place those counts are shown) re-reads a movie from recs-model only
// when its row is older than the refresh TTL (DefaultRefreshTTL, 10 minutes),
// and writes the row only when something changed. An unchanged re-check is
// remembered in memory, so anonymous page views cost neither recs-model calls
// nor database writes more than once per movie per TTL.
//
// Owner: workstream J. The table is created by migrations/0002_movies.sql.
package moviecache

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"log/slog"
	"reflect"
	"sync"
	"time"

	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
)

// Cache is the SQLite-backed movie metadata cache. It is safe for
// concurrent use.
type Cache struct {
	db     *db.DB
	model  modelclient.Client
	now    func() time.Time
	logger *slog.Logger
	ttl    time.Duration

	mu      sync.Mutex
	checked map[int64]time.Time // unchanged re-checks since the row was written
}

// DefaultRefreshTTL is how long a row counts as fresh for Refresh.
const DefaultRefreshTTL = 10 * time.Minute

// maxChecked bounds the in-memory re-check map; it is cleared when full
// (the cost is at most one extra recs-model call per movie).
const maxChecked = 10_000

// Option configures a Cache.
type Option func(*Cache)

// WithClock replaces time.Now (tests).
func WithClock(now func() time.Time) Option { return func(c *Cache) { c.now = now } }

// WithRefreshTTL sets how long a row counts as fresh for Refresh; zero
// makes Refresh always ask recs-model.
func WithRefreshTTL(d time.Duration) Option { return func(c *Cache) { c.ttl = d } }

// WithLogger sets the logger. The default is slog.Default(), which main sets
// to the process logger (app.Deps.Logger) before building the cache.
func WithLogger(l *slog.Logger) Option { return func(c *Cache) { c.logger = l } }

// New returns a Cache over database that fills misses from model.
func New(database *db.DB, model modelclient.Client, opts ...Option) *Cache {
	c := &Cache{db: database, model: model, now: time.Now, logger: slog.Default(), ttl: DefaultRefreshTTL,
		checked: map[int64]time.Time{}}
	for _, o := range opts {
		o(c)
	}
	if c.logger == nil {
		c.logger = slog.Default()
	}
	return c
}

// Result is what Get found.
type Result struct {
	// Movies holds every requested movie that is cached or that recs-model
	// returned, keyed by movie ID.
	Movies map[int64]modelclient.MovieMeta
	// Unknown lists requested IDs recs-model says do not exist.
	Unknown []int64
	// Unchecked lists requested IDs that are not cached and that recs-model
	// could not be asked about (Get's error says why).
	Unchecked []int64
}

// Get returns metadata for ids (duplicates allowed). Cached rows come from
// SQLite; the rest are fetched with /v1/movies/batch in chunks of at most
// modelclient.MaxBatchIDs, in first-seen order, and stored.
//
// If recs-model fails, Get still returns everything cached, lists the rest
// in Unchecked and returns the model error (errors.Is(err,
// modelclient.ErrUnavailable) when it is down). A database read error is
// returned with an empty Result.
func (c *Cache) Get(ctx context.Context, ids []int64) (Result, error) {
	res := Result{Movies: map[int64]modelclient.MovieMeta{}}
	ids = dedupe(ids)
	if len(ids) == 0 {
		return res, nil
	}
	cached, err := c.Lookup(ctx, ids)
	if err != nil {
		return res, err
	}
	res.Movies = cached
	var misses []int64
	for _, id := range ids {
		if _, ok := cached[id]; !ok {
			misses = append(misses, id)
		}
	}

	for start := 0; start < len(misses); start += modelclient.MaxBatchIDs {
		chunk := misses[start:min(start+modelclient.MaxBatchIDs, len(misses))]
		fetched, err := c.fetch(ctx, chunk)
		if err != nil {
			// Don't wait on a failing model once per chunk.
			res.Unchecked = append(res.Unchecked, misses[start:]...)
			return res, err
		}
		for _, id := range chunk {
			if m, ok := fetched[id]; ok {
				res.Movies[id] = m
			} else {
				res.Unknown = append(res.Unknown, id)
			}
		}
	}
	return res, nil
}

// Refresh returns one movie for the movie page. A cached row younger than
// the refresh TTL is returned as is. Otherwise recs-model is asked; the row
// is written only if the metadata changed (an unchanged answer just restarts
// the TTL, in memory). ok is false when recs-model says the movie does not
// exist. When recs-model can't be asked, the cached row (however old) is
// returned; with no cached row the model error is returned.
func (c *Cache) Refresh(ctx context.Context, id int64) (m modelclient.MovieMeta, ok bool, err error) {
	rows, err := c.lookupRows(ctx, []int64{id})
	if err != nil {
		return m, false, err
	}
	row, have := rows[id]
	now := c.now()
	if have && c.fresh(id, row.updatedAt, now) {
		return row.meta, true, nil
	}

	resp, _, err := c.model.MoviesBatch(ctx, []int64{id})
	if err != nil {
		if have {
			return row.meta, true, nil
		}
		return m, false, err
	}
	for _, got := range resp.Movies {
		if got.MovieID == id {
			m, ok = got, true
			break
		}
	}
	if !ok {
		return m, false, nil
	}
	if m.Genres == nil {
		m.Genres = []string{}
	}
	if have && reflect.DeepEqual(row.meta, m) {
		c.markChecked(id, now)
		return m, true, nil
	}
	if err := c.Put(ctx, []modelclient.MovieMeta{m}); err != nil {
		c.logger.WarnContext(ctx, "moviecache: storing movie failed", "err", err)
	}
	return m, true, nil
}

// fresh reports whether a row written at updatedAt (Unix seconds), or last
// re-checked unchanged, is younger than the TTL.
func (c *Cache) fresh(id, updatedAt int64, now time.Time) bool {
	last := time.Unix(updatedAt, 0)
	c.mu.Lock()
	if t, ok := c.checked[id]; ok && t.After(last) {
		last = t
	}
	c.mu.Unlock()
	return now.Sub(last) < c.ttl
}

func (c *Cache) markChecked(id int64, now time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.checked) >= maxChecked {
		clear(c.checked)
	}
	c.checked[id] = now
}

// fetch asks recs-model for ids (at most MaxBatchIDs) and stores what comes
// back. Only requested IDs are accepted from the response. A failure to
// store is logged, not returned: the caller still gets the metadata.
func (c *Cache) fetch(ctx context.Context, ids []int64) (map[int64]modelclient.MovieMeta, error) {
	resp, _, err := c.model.MoviesBatch(ctx, ids)
	if err != nil {
		return nil, err
	}
	want := make(map[int64]bool, len(ids))
	for _, id := range ids {
		want[id] = true
	}
	out := make(map[int64]modelclient.MovieMeta, len(resp.Movies))
	var keep []modelclient.MovieMeta
	for _, m := range resp.Movies {
		if !want[m.MovieID] {
			continue
		}
		if m.Genres == nil {
			m.Genres = []string{}
		}
		out[m.MovieID] = m
		keep = append(keep, m)
	}
	if err := c.Put(ctx, keep); err != nil {
		c.logger.WarnContext(ctx, "moviecache: storing movies failed", "count", len(keep), "err", err)
	}
	return out, nil
}

// Lookup returns the cached rows for ids without asking recs-model.
func (c *Cache) Lookup(ctx context.Context, ids []int64) (map[int64]modelclient.MovieMeta, error) {
	rows, err := c.lookupRows(ctx, ids)
	if err != nil {
		return nil, err
	}
	out := make(map[int64]modelclient.MovieMeta, len(rows))
	for id, r := range rows {
		out[id] = r.meta
	}
	return out, nil
}

// cachedRow is one movies row.
type cachedRow struct {
	meta      modelclient.MovieMeta
	updatedAt int64
}

func (c *Cache) lookupRows(ctx context.Context, ids []int64) (map[int64]cachedRow, error) {
	out := make(map[int64]cachedRow, len(ids))
	if len(ids) == 0 {
		return out, nil
	}
	// The ID list is bound as one JSON array parameter and expanded by
	// json_each, so the statement text never depends on the input.
	idsJSON, err := json.Marshal(ids)
	if err != nil {
		return nil, err
	}
	rows, err := c.db.Reader.QueryContext(ctx,
		`SELECT movie_id, title, year, genres_json, tmdb_id, rating_count, mean_rating, updated_at
		   FROM movies
		  WHERE movie_id IN (SELECT value FROM json_each(?))`, string(idsJSON))
	if err != nil {
		return nil, fmt.Errorf("moviecache: lookup: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var (
			m      modelclient.MovieMeta
			year   sql.NullInt64
			tmdb   sql.NullInt64
			mean   sql.NullFloat64
			genres string
			upd    int64
		)
		if err := rows.Scan(&m.MovieID, &m.Title, &year, &genres, &tmdb, &m.RatingCount, &mean, &upd); err != nil {
			return nil, fmt.Errorf("moviecache: scan: %w", err)
		}
		if year.Valid {
			y := int(year.Int64)
			m.Year = &y
		}
		if tmdb.Valid {
			v := tmdb.Int64
			m.TMDBID = &v
		}
		if mean.Valid {
			v := mean.Float64
			m.MeanRating = &v
		}
		if err := json.Unmarshal([]byte(genres), &m.Genres); err != nil || m.Genres == nil {
			m.Genres = []string{}
		}
		out[m.MovieID] = cachedRow{meta: m, updatedAt: upd}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("moviecache: lookup: %w", err)
	}
	return out, nil
}

// Put upserts movies in one short transaction.
func (c *Cache) Put(ctx context.Context, movies []modelclient.MovieMeta) error {
	if len(movies) == 0 {
		return nil
	}
	now := c.now().Unix()
	tx, err := c.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	stmt, err := tx.PrepareContext(ctx,
		`INSERT INTO movies (movie_id, title, year, genres_json, tmdb_id, rating_count, mean_rating, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT (movie_id) DO UPDATE SET
		   title = excluded.title, year = excluded.year, genres_json = excluded.genres_json,
		   tmdb_id = excluded.tmdb_id, rating_count = excluded.rating_count,
		   mean_rating = excluded.mean_rating, updated_at = excluded.updated_at`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for _, m := range movies {
		genres := m.Genres
		if genres == nil {
			genres = []string{}
		}
		g, err := json.Marshal(genres)
		if err != nil {
			return err
		}
		if _, err := stmt.ExecContext(ctx, m.MovieID, m.Title, m.Year, string(g), m.TMDBID, m.RatingCount, m.MeanRating, now); err != nil {
			return fmt.Errorf("moviecache: store movie %d: %w", m.MovieID, err)
		}
	}
	return tx.Commit()
}

// dedupe returns ids without duplicates, keeping first-seen order.
func dedupe(ids []int64) []int64 {
	seen := make(map[int64]bool, len(ids))
	out := make([]int64, 0, len(ids))
	for _, id := range ids {
		if !seen[id] {
			seen[id] = true
			out = append(out, id)
		}
	}
	return out
}
