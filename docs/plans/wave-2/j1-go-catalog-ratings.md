# J1 — recs-api: model client, movie cache, movies, ratings, export

**Owns:** `api/internal/modelclient/http.go` (+ tests), `api/internal/{moviecache,movies,ratings,export}/`,
`api/migrations/0002_*.sql` (next number), `api/cmd/recs-api/export*.go` if needed.
**Builds on:** I1 (merged or merge `wave1/i1-go-foundation`): `auth.RequireSession`, `auth.UserID(ctx)`,
`httpx.DecodeJSON`, `apperr`, `db` pools, `ratelimit`. The model client interface and fake are frozen
(`api/internal/modelclient/client.go`, `fake/`).
**Contracts:** `contracts/public.yaml` — `searchMovies`, `getMovie`, `getSimilar`, `getOnboarding`,
`listRatings`, `putRating`, `deleteRating`, `listModels`; `contracts/model.yaml`; `contracts/export.md`;
fixtures in `contracts/fixtures/{public,model}/`.
**Spec:** "HTTP API", "Accounts → Input limits / Concurrency", "Recommendation cache" (ratings_version).

## Build
1. `modelclient.NewHTTP`: real client over a Unix socket (or TCP addr). 500 ms deadline via context;
   a single retry ONLY for idempotent reads and ONLY on connection errors; passes `X-Request-ID`
   (from `reqid`); `ErrUnavailable` on conn error / timeout / 503; `StatusError` for other non-2xx;
   strict JSON decoding; response size cap (e.g. 4 MiB). Test against `fake.NewServer()` (incl. a
   Unix-socket listener test) for every operation, plus timeout, retry, and no-retry-on-POST tests.
2. `moviecache`: SQLite table (migration 0002) `movies(movie_id PK, title, year, genres_json,
   tmdb_id, rating_count, mean_rating, updated_at)`. `Get(ctx, ids)` returns cached rows, fetches
   misses via `MoviesBatch` (≤ 200 per call) and upserts them. When the model is down, cached rows
   still come back; misses are reported. A model-version change doesn't invalidate rows (metadata is
   stable; counts may lag — document it).
3. `movies` routes: search (delegates to model, enriches via moviecache), get (with caller's
   user_rating when a session is present; `Cache-Control: private, no-store` then), similar
   (model ∈ {knn, content}), onboarding (exclude the user's rated movies), models.
4. `ratings` routes: list (newest first, titles from moviecache — works with the model down), PUT
   (validate 0.5 steps; movie must exist in moviecache or via MoviesBatch; 404 unknown; 503 only if
   unknown to the cache AND model down; ≤ 5,000 ratings per user → 409 too_many_ratings; upsert and
   `users.ratings_version += 1` in ONE transaction), DELETE (idempotent 204; bumps ratings_version
   only if a row was deleted).
5. `export-ratings` subcommand: exactly `contracts/export.md` (temp file + rename, LATEST pointer,
   keep 7, no usernames). Test that deleted accounts are absent and the file has the exact header.

## Gate tests
- Contract fixtures replay for every owned operation (status, body shape, headers).
- With the model fake `SetDown(true)`: list ratings and PUT/DELETE on cached movies still work
  (Phase 2 degradation gate); model routes return 503 `model_unavailable` within 1 s.
- Ratings: 0.5-step validation, 5,000 cap, ratings_version bump in the same transaction
  (concurrent PUTs with `-race` → ratings_version equals number of successful writes).
- SQL-injection corpus as search query is passed through literally; no string-built SQL.
- Go native fuzz targets for every JSON decoder you add.
