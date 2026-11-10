# E — Content-based model (phase 1)

**Owns:** `core/src/models/content.cpp` (+ optional `content_*.cpp`/`content_detail.hpp`), `tests/core/models/content/`
**Implements:** `make_content()`, a `Model` named `"content"` (frozen `recs/model.hpp` — read its contract).
**Spec:** "Models → Content-based"; Phase 1 gates. Protocol in `docs/plans/wave-1/README.md` applies.

## Math (comment block with references at the top of content.cpp)
- Item vector v_i: tag-genome relevance row if `Dataset::genome` is present; otherwise (ml-32m, ml-latest-small)
  TF-IDF over the item's `tags` (case-folded, whitespace-trimmed tag strings as terms; ignore terms used on
  fewer than `min_tag_count` distinct items, default 2) concatenated with one-hot genres scaled by
  `genre_weight` (default 1.0); then L2-normalize. Items with no features have a zero vector.
- Tags come from `train.tags` only (the split already drops a user's tags on their own held-out movies).
- User profile: p_u = Σ_j (r_uj − μ_u) v_j over rated movies (μ_u = the user's mean in the given ratings).
- Score: cosine(p_u, v_i); −∞ for items with a zero vector or when p_u = 0.
- explain(u, i): the rated movie j with the largest contribution (r_uj − μ_u)·(v_j · v_i)/(‖p_u‖‖v_i‖)
  (top 3, positive only).
- similar(i, k): cosine nearest items (positive only), `supports_similar() = true`.
- Batch path: `supports_trained_scoring() = true`; `score_trained_user(u)` builds the profile from the
  training CSR row with the same arithmetic order as fold_in (must be bit-identical).
- Sparse feature vectors (CSR over items × features); profile as a dense double vector; scoring as a sparse
  mat-vec with OpenMP static schedule; deterministic.
- save/load: binary feature matrix + vocabulary sizes + config; little-endian with a header; hostile-input safe.

## Gate tests (write first)
1. Synthetic one-genre user (spec gate): on a catalog where a user rated only movies of one genre, ≥ 7 of the
   top-10 recommendations share that genre. Build the fixture so the result is not trivially guaranteed
   (several genres, overlapping genres, tags).
2. fold_in + score equals the batch path EXACTLY (bitwise) for every training user (tiny + random datasets).
3. Explanations only cite items the user rated (property test).
4. Hand fixture: 3 items × small vocab, TF-IDF and cosine computed by hand in a comment.
5. TF-IDF details: case-folding, min_tag_count, empty-feature items score −∞, genome path used when present.
6. Determinism 1 vs 4 threads; save/load round-trip bitwise identical scores.

At the end: fit time and memory on ml-32m (release, OMP_NUM_THREADS=4) — the only ml-32m job you run.
