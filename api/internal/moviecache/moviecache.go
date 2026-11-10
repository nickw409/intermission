// Package moviecache is recs-api's own copy of movie metadata, stored in
// SQLite (movie_id, title, year, genres, tmdb_id, rating_count, mean_rating,
// updated_at). It is filled from every /v1/movies/batch response (search,
// onboarding and recommend results are enriched through batch), and ratings
// PUT/GET read from it, so ratings keep working while recs-model is down.
//
// Owner: workstream J. The table itself is added by a migration in
// api/migrations.
package moviecache

import (
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/modelclient"
)

// Cache is the SQLite-backed movie metadata cache.
//
// TODO(workstream J): add lookup and fill methods.
type Cache struct {
	db    *db.DB
	model modelclient.Client
}

// New returns a Cache over database that fills misses from model.
func New(database *db.DB, model modelclient.Client) *Cache {
	return &Cache{db: database, model: model}
}
