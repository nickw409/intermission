-- 0002_movies: recs-api's movie metadata cache (internal/moviecache).
--
-- Filled from every recs-model /v1/movies/batch response, so pages that only
-- need titles (the ratings list, rating a movie the site has shown) keep
-- working while recs-model is down. Rows are never invalidated when the model
-- version changes: titles, years, genres and TMDB IDs are stable catalog
-- data. rating_count and mean_rating may lag until the row is refreshed (the
-- movie page refreshes its row on every view while recs-model is up).

CREATE TABLE movies (
  movie_id      INTEGER PRIMARY KEY CHECK (movie_id BETWEEN 1 AND 2147483647),
  title         TEXT NOT NULL,
  year          INTEGER,                       -- NULL when the title has no year
  genres_json   TEXT NOT NULL CHECK (json_valid(genres_json) AND json_type(genres_json) = 'array'),
  tmdb_id       INTEGER,                       -- NULL when links.csv has none
  rating_count  INTEGER NOT NULL CHECK (rating_count >= 0),
  mean_rating   REAL,                          -- NULL when unrated
  updated_at    INTEGER NOT NULL               -- Unix seconds of the last fill
);
