# D — Item-KNN (phase 1)

**Owns:** `core/src/models/item_knn.cpp` (+ optional `core/src/models/item_knn_*.cpp` helpers), `tests/core/models/item_knn/`
**Implements:** `make_item_knn()` and a `Model` subclass named `"knn"` (frozen `recs/model.hpp`). Read the
Model contract comment at the top of model.hpp carefully, plus `recs/types.hpp`, `recs/sparse.hpp`,
`recs/dataset.hpp`, `recs/metrics.hpp`, `configs/knn.json`.
**Spec:** "Models → Item-KNN"; Phase 1 gates.
**Protocol:** `docs/plans/wave-1/README.md` applies (ownership, tests first, never loosen thresholds,
build flags, commits on your branch only, no AI mentions).

## Math (put a comment block with references at the top of item_knn.cpp)
- Adjusted cosine on mean-centered ratings: center each rating by its USER's mean
  (r_uj − μ_u), then s_ij = Σ_u c_ui c_uj / (sqrt(Σ_u∈U_i c_ui²) sqrt(Σ_u∈U_j c_uj²)), sums over users who rated both
  in the numerator; document whether the norms are over co-raters or all raters (choose co-raters — the
  standard Sarwar et al. 2001 definition — and say so).
- Shrinkage: s'_ij = s_ij · n_ij / (n_ij + λ), λ = `shrinkage` (default 100).
- Keep the top M (`neighbors`, default 100) by |s'| — or by s' if you justify it — per item; only
  items with ≥ `min_support` (default 20) ratings get neighbour lists AND may appear as neighbours.
- Score: ŝ_ui = Σ_{j ∈ N(i) ∩ R(u)} s'_ij (r_uj − μ_u) / Σ |s'_ij|; items with no rated neighbour
  score −∞ (never recommended). Optionally add μ_u to report a rating-scale score — document it.
- fold_in(ratings): UserState{ratings, mean}; score() uses only the item-side neighbour lists.
- Batch path: `supports_trained_scoring() = true`; `score_trained_user(u)` computes the same formula
  directly from the training CSR row of user u (fit() must keep a reference/copy of what it needs).
- explain(): the rated items with the largest s'_ij·(r_uj − μ_u) contributions (top 3, positive only).
- similar(item, k): the neighbour list, `supports_similar() = true`.
- save/load: binary neighbour lists + config, little-endian with a small header.
- Performance target: ml-32m (87,585 items, 32M ratings) fit in minutes, not hours, on 8 cores with
  bounded memory. Item-item via the item×user CSC with a per-thread dense accumulator over candidate
  items is the usual approach; restrict to items with ≥ min_support. Deterministic: static OpenMP
  schedule; reductions in a fixed order; ties broken by ItemIdx.

## Gate tests (write first)
1. Symmetry and bounds: on random datasets (property, ≥ 100 seeds) the full (pre-truncation) shrunk
   similarity is symmetric (s'_ij == s'_ji exactly or within 1e-12 — say which and why) and in [−1, 1];
   |s'| ≤ |s| always (shrinkage never increases magnitude). Expose a test-only function that returns the
   untruncated similarity for a pair (e.g. in an `item_knn_detail.hpp` you own).
2. Hand fixture: 3–4 users × 3 items, similarity and score computed by hand in a comment.
3. fold_in + score equals the batch path EXACTLY (bitwise) for every training user of the tiny dataset
   and of random datasets — same arithmetic, same order.
4. Explanations only cite items the user rated (property test).
5. min_support respected; neighbours truncated to M; save/load round-trip gives identical scores.
6. Determinism: identical neighbour lists with OMP_NUM_THREADS=1 and 4.

## After merge (orchestrator)
ml-32m: KNN must beat popularity on NDCG@10 with a paired-bootstrap delta excluding zero (Phase 1 gate).
