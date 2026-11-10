# A1 — CSV parser, MovieLens ingest, binary cache

**Owns:** `core/src/data/csv.cpp`, `core/src/data/movielens.cpp`, `core/src/data/cache.cpp`, `train/cmd_ingest.cpp`, `tests/core/data/`
**Implements (frozen headers):** `recs/csv.hpp`, and in `recs/dataset.hpp`: `load_movielens`, `save_cache`, `load_cache`, `load_or_build`. Read `recs/types.hpp` first.
**Already done by the spine (use them, don't reimplement):** `csr_from_triplets`, `transpose`, `validate` (`core/src/sparse/`), `make_dataset` (`core/src/data/make_dataset.cpp`), with the CSR round-trip property test in `tests/core/spine/sparse_test.cpp`. `merge_site_ratings` belongs to phase 5 (leave its stub).
**Spec:** "Data" section; Phase 0 gates 1–3.

## Gate tests (write first)
1. `tests/core/data/csv_test.cpp` — every file in `tests/fixtures/csv/` behaves exactly as its
   README says (field values, record counts, and for malformed files: `CsvError` with the exact
   `row()`). Plus inline cases: empty field `a,,c`; empty quoted field `""`; a lone `\r` inside
   quotes is literal; a field that is only `""""` → `"`.
2. (Done in the spine: CSR → CSC → CSR property test.) Add one test that the Dataset built by
   `load_movielens` passes `validate()` for both `by_user` and `by_item`.
3. `tests/core/data/movielens_test.cpp` on `tests/fixtures/tiny/`: 245 ratings, 30 movies,
   20 users, 11 tags; movie 4 title `American President, The`, year 1995; movie 26 title
   `"Great Performances" Cats`; movie 27 has no year and title `Hamlet, Prince of Denmark`;
   movie 28 has empty genres; movie 30 has zero ratings but an ItemIdx; links: movie 27 has no
   imdb/tmdb id; `by_item == transpose(by_user)`; items are indexed in movies.csv order and users
   in order of first appearance. A rating for an unknown movie, a rating of 0.0 or 5.5 or 3.25, and
   a duplicate (user, movie) each throw with file name and row number (build these as small temp
   CSV dirs in the test).
4. ml-latest-small (skip with `SKIP()` if `data/raw/ml-latest-small` is absent): 100,836 ratings,
   9,742 movies, 610 users, 3,683 tags. Titles like `From Dusk Till Dawn 2: Texas Blood Money (1999) `
   (trailing space) still parse the year; `Babylon 5` has no year.
5. `[ml32m]`-tagged test (excluded from default ctest; run manually in release): loading
   `data/raw/ml-32m` yields exactly 32,000,204 ratings, 87,585 movies, 200,948 users,
   2,000,072 tags. Print the wall-clock time for parse and for cache load.
6. Cache: `save_cache` → `load_cache` round-trips a Dataset exactly (all fields, incl. genome when
   present — build a small synthetic genome in the test). Flipping any single byte, truncating, or
   changing the version makes `load_cache` throw `DatasetError`. `load_or_build` uses a valid cache
   and rebuilds a corrupt one.

## Implementation notes
- Year: trailing `(YYYY)` after trimming whitespace; title is the text before it, trimmed. Anything
  else (`(1994-1998)`, no parentheses) → no year, title unchanged (trimmed).
- Performance: ml-32m parse should take on the order of a minute in release, cache load a few
  seconds. Avoid per-field heap allocations in the hot path if it matters; measure before optimizing.
- `recs-train ingest --dataset <name> [--raw data/raw/<name>] [--cache data/cache/<name>.bin]`
  parses and writes the cache, printing counts.
