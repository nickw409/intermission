# J2 — recs-api: recommendations, cache, reports, posters

**Owns:** `api/internal/{recommend,reports,posters}/`.
**Builds on:** I1 (auth, httpx, ratelimit) and J1 (`modelclient.NewHTTP`, `moviecache`, ratings table).
**Contracts:** `contracts/public.yaml` — `recommend`, `compare`, `listReports`, `getReport`, `getPoster`;
`contracts/model.yaml` (`version`, `recommend`, `compare`); fixtures.
**Spec:** "HTTP API" (recommendation response, cold start, posters), "Recommendation cache",
rate limits (/recommend 30/min per session).

## Build
1. `recommend`: GET /api/recommend and /api/recommend/compare. Load the user's ratings; < 5 ratings →
   `cold_start: true` with popularity results (`pop`). Call `GET /v1/version` before every cache
   lookup; LRU cache keyed by (user_id, ratings_version, model_version, model_name[s], k), bounded
   size; enrich titles/years via moviecache (incl. `because` items). Per-session rate limit 30/min via
   `ratelimit.Middleware` + `auth.SessionKey`. Default model from `/v1/models` `default_model`.
2. `reports`: list `RECS_REPORTS_DIR/*.json` (top level only; never `reports-private`), newest first,
   summaries per `ReportSummary` (unknown schema_version → nulls); GET by run_id only if the id is in
   the current listing (exact match; no path built from input); file size cap; cache listing for a few
   seconds.
3. `posters`: GET /api/posters/{movieId}. TMDB id from moviecache; fetch poster path via TMDB API
   (key from config, never logged, never sent to the browser), then the w342 image; only TMDB hosts,
   no redirects to other hosts; response size cap; disk cache with size cap + LRU eviction (file names
   from movie id only); global upstream rate limit; ANY failure → SVG placeholder with 200 (never 5xx).

## Gate tests
- Contract fixtures replay for every owned operation.
- Cache never serves a response computed by an older model version (fake changes version between calls).
- cold_start below 5 ratings; rate limit 30/min per session triggers 429.
- Degradation: model down → 503 `model_unavailable` within 1 s.
- Reports: path traversal attempts (`..`, `%2e%2e`, `.per_user`, absolute paths) → 400/404; only
  listed IDs served; private dir never readable.
- Posters: TMDB down / 404 / huge response / redirect to another host → placeholder 200; key never
  appears in logs or responses; cache eviction keeps total size under the cap.
