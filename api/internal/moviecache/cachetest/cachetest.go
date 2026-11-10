// Package cachetest holds test helpers shared by the moviecache, movies,
// ratings and export tests: a migrated temporary app.db, users with live
// sessions, and seeded cache rows. It is imported only from _test.go files.
package cachetest

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"io"
	"log/slog"
	"path/filepath"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
)

// OpenDB returns a migrated database in a temporary directory, closed when
// the test ends.
func OpenDB(t testing.TB) *db.DB {
	t.Helper()
	ctx := context.Background()
	d, err := db.Open(ctx, filepath.Join(t.TempDir(), "app.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = d.Close() })
	if err := d.Migrate(ctx, slog.New(slog.NewTextHandler(io.Discard, nil))); err != nil {
		t.Fatal(err)
	}
	return d
}

// NewUser inserts a user and a live session and returns the user ID and the
// raw session token (cookie value). The token format and its stored SHA-256
// mirror internal/auth.
func NewUser(t testing.TB, d *db.DB, username string) (int64, string) {
	t.Helper()
	res, err := d.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', ?)", username, time.Now().Unix())
	if err != nil {
		t.Fatal(err)
	}
	uid, err := res.LastInsertId()
	if err != nil {
		t.Fatal(err)
	}
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		t.Fatal(err)
	}
	tok := base64.RawURLEncoding.EncodeToString(raw[:])
	sum := sha256.Sum256([]byte(tok))
	now := time.Now().Unix()
	if _, err := d.Writer.Exec("INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
		sum[:], uid, now, now+3600); err != nil {
		t.Fatal(err)
	}
	return uid, tok
}

// SeedMovies writes rows straight into the movie cache.
func SeedMovies(t testing.TB, d *db.DB, movies ...modelclient.MovieMeta) {
	t.Helper()
	for _, m := range movies {
		genres := m.Genres
		if genres == nil {
			genres = []string{}
		}
		g, _ := json.Marshal(genres)
		if _, err := d.Writer.Exec(
			"INSERT OR REPLACE INTO movies (movie_id, title, year, genres_json, tmdb_id, rating_count, mean_rating, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
			m.MovieID, m.Title, m.Year, string(g), m.TMDBID, m.RatingCount, m.MeanRating); err != nil {
			t.Fatal(err)
		}
	}
}

// Movie returns a simple MovieMeta for id.
func Movie(id int64, title string, year int) modelclient.MovieMeta {
	return modelclient.MovieMeta{MovieID: id, Title: title, Year: &year, Genres: []string{"Drama"}, RatingCount: 1}
}

// RatingsVersion returns users.ratings_version for uid.
func RatingsVersion(t testing.TB, d *db.DB, uid int64) int64 {
	t.Helper()
	var v int64
	if err := d.Reader.QueryRow("SELECT ratings_version FROM users WHERE id = ?", uid).Scan(&v); err != nil {
		t.Fatal(err)
	}
	return v
}
