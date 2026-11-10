# H — recs-model: the internal model service (phase 2)

**Owns:** `model-service/**` (all of it; `model-service/CMakeLists.txt` is yours too), `tests/model-service/`,
`tests/fuzz/model/`.
**Builds on:** librecs (C1: `load_artifact`, `read_current`, `recommend`, `Catalog`; models via the registry).
**Contract:** `contracts/model.yaml` (every operation and rule), `contracts/artifact.md`, fixtures
`contracts/fixtures/model/` (shapes/status/headers — values depend on the trained model).
**Spec:** "System architecture", "Internal model API", Phase 2 gates (fuzzing, robustness), Phase 5 hot swap
(build the swap holder now; the watcher can be minimal).

## Build
- cpp-httplib server on a Unix domain socket (`--socket /run/recs/model.sock`, mode 0660) or TCP fallback
  (`--addr 127.0.0.1:8090`); `--artifacts data/artifacts`.
- Startup: `read_current` → `load_artifact` (verified) → serve. No verified artifact → every route except
  `/healthz` and `/metrics` returns 503 `model_unavailable` with `X-Model-Version: none`; healthz 503.
- State holder: `std::atomic<std::shared_ptr<const Bundle>>` (or a mutex-guarded shared_ptr) with the artifact,
  catalog-derived search index and onboarding list; requests take a snapshot at start. A watcher thread polls
  `CURRENT` (every 2 s) and swaps in a newly verified artifact; on failure it keeps the old one and logs.
- Handlers for every operation in model.yaml, with ALL validation rules (≤ 5,000 ratings, 0.5 steps,
  duplicate movie_id → 400, unknown ids skipped and counted in `ignored`, k ≤ 100, ≤ 200 batch ids,
  body ≤ 1 MiB, strict JSON — unknown fields rejected, wrong types → 400 `bad_request`, malformed → 400
  `invalid_json`). Every response carries `X-Model-Version`; `X-Request-ID` echoed (validated charset) or
  generated.
- `/v1/recommend` & `/v1/compare`: movie_id → ItemIdx, fold_in, recommend(k, exclude), explain → because
  (movie ids), ≤ 3 per item. `/v1/similar`: knn/content `similar()` (400 unknown_model if not loaded).
- Search: case- and accent-insensitive (fold UTF-8 Latin accents to ASCII; lowercase), prefix matches first
  (title or title without leading article "The/A/An" reordering — document), then substring, ties by
  popularity rank; precomputed normalized titles.
- Onboarding: popular and genre-diverse (e.g. round-robin over top genres by popularity), excluding `exclude`.
- `/metrics`: Prometheus text (request counts/latency by route, model version, fold-in latency, artifact load
  time) — hand-written exposition, no extra dependency. Logs: JSON lines, never rating payloads.
- Structure for testability: route handlers are plain functions `Response handle(const Request&, Snapshot)`
  in a library (`recs_model_service`), so tests and libFuzzer call them without sockets.

## Gate tests (write first)
- Contract: for every operation, responses validate against model.yaml shapes (use a minimal JSON-shape
  checker or the C1 schema_check helper pattern), correct statuses for every error case in the fixtures,
  `X-Model-Version` on every response (incl. errors), request-id echo.
- Validation matrix (each rule above) → clean 4xx, never 5xx, never crash.
- Hot swap: under concurrent requests (e.g. 8 threads hammering /v1/recommend), swap the artifact twice;
  zero failed requests, every response has exactly one X-Model-Version from the set {old, new}.
- Corrupted new artifact (bad checksum / partial) → rejected, old version keeps serving.
- No artifact → 503 everywhere except healthz/metrics.
- libFuzzer harnesses in `tests/fuzz/model/` for the request handlers (recommend, compare, batch, onboarding,
  search query, similar path); build with the `fuzz` preset; run each for 60 s locally and report (the
  1 CPU-hour run is scheduled later).
- Build a tiny artifact in tests via `recs-train`-equivalent library calls (pop + pop_count; knn/content if
  their branches are merged — otherwise pop only).
