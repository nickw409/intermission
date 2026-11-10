# B1 — Temporal split, ranking/rating metrics, bootstrap statistics

**Owns:** `core/src/split/`, `core/src/eval/`, `tests/core/split/`, `tests/core/eval/`
**Implements (frozen headers):** `recs/split.hpp`, `recs/metrics.hpp`, `recs/stats.hpp`. Read their
header comments carefully — they define the protocol exactly. Also `recs/rng.hpp`, `recs/sha256.hpp`.
**Spec:** "Evaluation protocol"; Phase 0 gates 4–5.
**Ready to use:** `make_dataset`, `csr_from_triplets`, `transpose` (spine, already implemented).

## Gate tests (write first)
1. **Split invariant (property, ≥ 200 seeds):** random datasets (random users/items/ratings,
   timestamps with many ties). For every kept user: every test ts ≥ every train ts, every
   validation ts ≥ every train-minus-val ts and ≤ every test ts; no (user, item) in two sets;
   train ∪ test = the user's ratings; train_minus_val ∪ validation = train; set sizes follow the
   clamp/round rule (test: test_min/test_max; validation: val_min/val_max); users with
   < min_ratings keep ALL their ratings in train and train_minus_val, have no validation/test
   ratings, and are not in eval_users. The hash follows the exact encoding in split.hpp. `hash` is identical for identical
   inputs+config and changes when the seed, any config field, or the set assignment of any
   rating changes (rating values are not hashed — see split.hpp). `cold_item_share` matches a
   brute-force recount.
2. **Metric fixtures:** for EACH of ndcg_at_k, recall_at_k, precision_at_k,
   average_precision_at_k: ≥ 5 hand-computed fixtures, with the hand computation written out in a
   comment, covering: no relevant items, all relevant, ties in held-out ratings (equal gains),
   k greater than the ranked list length, relevant items ranked below k, ratings exactly at the
   4.0 threshold and at 3.5. Same for jaccard_at_k (incl. both empty), catalog_coverage,
   popularity_ranks (ties → lower ItemIdx first), mean_popularity_rank, rating_errors, and
   roc_auc (perfect separation = 1, reversed = 0, all ties = 0.5, excluded items ignored).
3. **Bootstrap:** constant input → CI collapses to the constant; mean is the sample mean;
   deterministic for a seed; CI width shrinks ~1/sqrt(n) (check n vs 4n within tolerance);
   paired_bootstrap of identical vectors → excludes_zero false; a + 0.5 vs a → excludes_zero true;
   bootstrap_ratio_ci with sqrt_result reproduces RMSE on a fixture. Coverage sanity: over 200
   simulated normal samples (n = 200), the 95% CI contains the true mean 88–99% of the time.

## Implementation notes
- Accumulate in double. Use `recs::Rng`, never `<random>` distributions.
- Split must be fast on ml-32m (O(n log n) per user, OpenMP over users with static schedule if
  you parallelize; output must not depend on thread count — test with 1 and 4 threads).
- The split hash input format: document it in a comment (config fields, then for each user in
  index order, each rating's (item, set) in sorted order), so it is reproducible.
