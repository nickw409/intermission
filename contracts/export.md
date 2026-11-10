# Website ratings export (format_version 1)

FROZEN after Wave 0. Written by `recs-api export-ratings` (workstream J), read by
`recs-train retrain` via `merge_site_ratings` (workstream P). This file is the whole interface;
recs-train never opens app.db.

- Location: `data/exports/ratings-<UTC yyyy-mm-ddTHH-MM-SSZ>.csv`, written to a temp name and
  renamed into place; `data/exports/LATEST` holds the newest file name.
- Encoding: UTF-8, LF line endings, RFC 4180, header row, no quoting needed (all numeric).
- Columns, exactly: `site_user_id,movie_id,rating,timestamp`
  - `site_user_id`: recs-api's internal integer user ID (`users.id`). **Never** a username.
  - `movie_id`: MovieLens movieId.
  - `rating`: 0.5–5.0 in 0.5 steps, printed with one decimal (`4.5`).
  - `timestamp`: Unix seconds (`ratings.updated_at`).
- Rows sorted by (site_user_id, movie_id). Deleted accounts are absent (their rows cascade away
  before export). Only users with at least one rating appear.
- Retention: recs-api keeps the 7 newest exports and deletes older ones.
