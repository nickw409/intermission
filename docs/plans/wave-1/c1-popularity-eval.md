# C1 — Popularity models, recommend(), catalog + artifacts, evaluator + reports, train/eval CLI

**Owns:** `core/src/models/popularity.cpp`, `core/src/models/recommend.cpp`, `core/src/artifact/`,
`core/src/report/`, `train/cmd_train.cpp`, `train/cmd_eval.cpp`, `train/cmd_compare.cpp`, `tests/core/models/popularity/`,
`tests/core/recommend/`, `tests/core/artifact/`, `tests/core/report/`, `tests/core/integration_tiny/`
**Implements (frozen headers):** `recs/model.hpp` (recommend + the two popularity factories),
`recs/artifact.hpp`, `recs/eval.hpp`. Read `contracts/artifact.md` and `contracts/report.schema.json`.
**Spec:** "Models → Popularity", "Evaluation protocol" (run report), Phase 0 gates 6–7.
**Ready to use:** `make_dataset`, `csr_from_triplets`, `transpose` (spine).
**Dependencies:** A1 (load_movielens, cache) and B1 (split, metrics, stats) run in parallel with
you. Do popularity, recommend, catalog, artifacts, the report/per-user writers, rating baselines
and compare_runs first. The orchestrator will merge A1 and B1 into `main`; then rebase
onto `main` (`git rebase main`) and finish evaluate(), the CLI and the end-to-end tests.

## Gate tests (write first)
1. **Popularity:** damped mean `(n_i·mean_i + m·C) / (n_i + m)` with C = global mean (double
   accumulation), m from config (default 10); items with no ratings score C; `pop_count` scores
   n_i. Hand-computed fixture for both. fold_in ignores nothing but scores don't depend on the user.
   explain() returns no `because` entries. save/load round-trip gives identical scores.
2. **recommend():** excludes `exclude` and every item in u.ratings; order = score desc then ItemIdx
   asc; never returns -inf; k > candidates returns all candidates; k = 0 → empty. Property test vs
   a brute-force full sort (≥ 200 seeds).
3. **Artifacts:** write → load round-trip (catalog + models produce identical scores);
   flipping a byte in any file, deleting a file, adding an unlisted file, or a truncated manifest →
   `ArtifactError`; a leftover `.tmp-*` dir is ignored; write_current/read_current round-trip and
   CURRENT is replaced via rename.
4. **Metric scope:** ranking-metric means use only users with ≥ 1 relevant item;
   mean_pop_rank@10 and coverage@10 use every evaluated user. Fold-in parity
   (`foldin_jaccard@10`) runs only when `m.supports_trained_scoring()`: for a seeded sample of
   `parity_users` training users, Jaccard@10 between recommend() on `score_trained_user` and on
   `fold_in(train ratings)`; otherwise null. Every reduction is serial in ascending user order.
5. **Rating baselines:** `evaluate_rating_baselines` (global mean; user bias; item bias;
   user+item bias — biases damped like MF fold-in, damping 10, documented) with hand-computed
   fixtures; `make_report` fills `rating_baselines` when given, else null.
6. **Per-user + compare:** `write_per_user` writes `contracts/per_user.schema.json` files to
   `data/reports-private/` (never `data/reports/`); `compare_runs` of a file with itself →
   every mean_delta 0 and excludes_zero false; mismatched split_hash → throws; output validates
   against `contracts/comparison.schema.json`.
7. **Reports:** `make_report` output validates against `contracts/report.schema.json` (check the
   required keys and types in C++; CI also validates real output with the Node validator — write
   reports from the tiny integration test to `build/<preset>/test-reports/` so CI can pick them up).
8. **Determinism gate (spec Phase 0):** two `evaluate`+`make_report` runs with the same seeds on
   the tiny dataset give byte-identical JSON after removing exactly run_id, created_at,
   train_seconds, eval_seconds, peak_rss_bytes (the list frozen in report.schema.json). Also
   identical with OMP_NUM_THREADS=1 vs 4.
9. **End-to-end on tiny** (`tests/core/integration_tiny/`): load_movielens(tiny) → make_split →
   fit pop on split.train → evaluate on test → report; and on ml-latest-small if present.

## CLI
- `recs-train eval --dataset <name> --model <pop|pop_count> [--config configs/<model>.json] [--on test|validation] [--split-seed 42] [--seed 42] [--bootstrap 1000] [--reports data/reports] [--keep-per-user]`:
  loads the cache (`data/cache/<name>.bin`, building it if missing), splits, fits on `train`
  (or `train_minus_val` for `--on validation`), evaluates, writes `<reports>/<run_id>.json`
  (+ `data/reports-private/<run_id>.per_user.json` with `--keep-per-user`). Records every seed, the git SHA (compile definition
  `RECS_GIT_SHA`), train/eval seconds and peak RSS.
- `recs-train train --dataset <name> --models pop,pop_count --default-model pop [--out data/artifacts] [--set-current]`:
  fits on ALL ratings (no split) and writes an artifact.

- `recs-train compare --a <run_id> --b <run_id> [--private data/reports-private] [--out data/reports/comparisons] [--resamples 1000] [--seed 42]`:
  paired bootstrap of two runs' per-user files; prints a table and writes the comparison JSON.

## After merge (orchestrator queues these; you may run the small one)
- Popularity NDCG@10 with CI on ml-latest-small (you) and on ml-32m (orchestrator, release build).
