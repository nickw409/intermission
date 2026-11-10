# Phase 0 — Foundation and evaluation harness

**Status: all gates pass** (2026-11-10). Built by workstreams S (spine), A1, B1 and C1; each merged after an
independent code review and test validation (mutation testing), with every finding fixed before merge.

## What was built
- CMake build with presets: `debug` (ASan + UBSan, warnings as errors), `release`, `fuzz` (clang + libFuzzer); every
  dependency pinned by version and SHA-256. CI runs all three plus Go, web, contracts and a gitleaks history scan.
- Ingest: RFC 4180 CSV reader, MovieLens loader with row-level validation, dense ID remapping, CSR/CSC, and a
  checksummed binary cache (v2: raw-file fingerprints, range checks, atomic writes).
- Temporal split with a seeded tie-break, validation carve-out, cold-item accounting, a leakage guard for tags,
  and a reproducible split hash.
- Metrics (NDCG, recall, precision, MAP @k; coverage; popularity rank; RMSE/MAE; Jaccard; ROC AUC) and percentile
  bootstrap CIs (incl. paired and ratio bootstraps), all accumulated in double.
- Popularity (damped mean) and popularity-count models, the shared `recommend()`, artifacts with verified
  manifests and an atomic `CURRENT` pointer, the evaluator, run reports, private per-user files, paired comparisons,
  and the `recs-train ingest | train | eval | compare` CLI.

## Gate results
| Gate | Result | Evidence |
|---|---|---|
| CSV parser: quoted commas, escaped quotes, BOM, CRLF, malformed row fails with its row number | ✅ | `tests/core/data/csv_test.cpp` against `tests/fixtures/csv/` (exact row numbers asserted) |
| ml-32m ingest yields exactly 32,000,204 ratings, 87,585 movies, 200,948 users | ✅ | `[ml32m]` test, release build (also 2,000,072 tags). Run locally — the data can't be in CI (decision 9) |
| CSR → CSC → CSR round-trip identical (property test) | ✅ | `tests/core/spine/sparse_test.cpp`, 300 seeds |
| Split invariant (test ts ≥ train ts; no pair in two sets) | ✅ | `tests/core/split/split_test.cpp`, 300-seed property test with heavy timestamp ties |
| Each metric matches ≥ 5 hand-computed fixtures (no relevant, all relevant, ties, k > candidates) | ✅ | `tests/core/eval/metrics_test.cpp`; 13 fixtures recomputed independently by the validator |
| Same seed → byte-identical reports apart from run_id and timing fields | ✅ | determinism tests, 1 vs 4 threads, incl. fold-in parity and rating paths |
| Popularity NDCG@10 with 95% CI recorded for ml-latest-small and ml-32m | ✅ | table below |

## Headline metrics (test split, split seed 42, 1,000 bootstrap resamples)
| Dataset | Model | NDCG@10 [95% CI] | Recall@50 | Coverage@10 | Cold-user NDCG@10 |
|---|---|---|---|---|---|
| ml-latest-small | pop (m = 10) | 0.0355 [0.0281, 0.0444] | 0.1146 | 0.0068 | — |
| ml-latest-small | pop_count | 0.0457 [0.0369, 0.0558] | 0.1513 | 0.0123 | — |
| ml-32m | pop (m = 10) | 0.0101 [0.0099, 0.0103] | 0.0611 | 0.0008 | 0.0179 [0.0170, 0.0189] |
| ml-32m | pop_count | 0.0440 [0.0435, 0.0445] | 0.1565 | 0.0051 | 0.0661 [0.0638, 0.0684] |

ml-32m: 200,948 users evaluated; cold-item share of test ratings 0.05%. Each eval took ~65–71 s wall (load from
cache, split, fit, evaluate, 1,000 bootstrap resamples) with ~3.5 GB peak RSS on 4 threads.

## Surprises
- **The damped mean (m = 10) is a much weaker baseline than the raw count** on both datasets (4× lower NDCG@10 on
  ml-32m). With only 10 pseudo-ratings of prior, movies with a few dozen perfect ratings outrank blockbusters. `m`
  should be tuned on the validation set; until then **pop_count is the baseline every model must beat**.
- **Tags leak held-out interactions** unless filtered: a user's tag on a test movie reveals they watched it. The
  split now drops those tags (decision 17).
- ml-latest-small's `movies.csv` has CRLF line endings and titles with trailing spaces before the year.
- Run IDs don't include the dataset, so two evals of one model on different datasets in the same second collide
  (follow-up logged).

## Decisions made during the phase
See the decisions table in `docs/plans/overnight-2026-11-10.md` (items 2–8, 16–20). The ones that touch the
evaluation protocol (2, 3, 4) need the owner's confirmation; none was made to improve any model's numbers.
