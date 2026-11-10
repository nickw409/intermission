# Phase 1 — Item-KNN and content-based

**Status: NOT signed off — awaiting owner decisions** (open questions 2 and 3 in
`docs/plans/overnight-2026-11-10.md`). All code is merged and reviewed; the numbers below are final for the
configurations shown.

## What was built
- Item-KNN (`core/src/models/item_knn.cpp`): adjusted cosine over co-raters with shrinkage, top-M neighbours,
  min-support, fold-in, an exact batch path, explanations, `similar()`. Two extra config switches,
  `normalize` (spec default `true`) and `positive_only` (default `false`), let the spec's rating-prediction score be
  compared with the standard top-N variant (Cremonesi et al. 2010).
- Content-based (`core/src/models/content.cpp`): TF-IDF over tags (ml-32m has no genome) + genre one-hots,
  L2-normalized; profile Σ(r − μ_u)v_j exactly as specified; cosine scoring; explanations; `similar()`.

## Gate results
| Gate | Result | Evidence |
|---|---|---|
| Similarity symmetric and in [−1, 1]; shrinkage never increases \|sim\| | ✅ | exact (bitwise) symmetry over 150 random datasets |
| fold_in + score equals the batch path exactly (KNN, content) | ✅ | memcmp over all modes and random datasets |
| Explanations cite only rated movies | ✅ | property tests |
| **Item-KNN beats popularity on NDCG@10 on ml-32m (paired bootstrap excludes zero)** | ⚠️ **only with the tuned top-N variant** | table below |
| **Content: one-genre user gets ≥ 7/10 in-genre** | ❌ **fails with the spec's formula** | 0–6/10 over 20 seeds; kept as a hidden `[.gate-open]` test |

## Item-KNN on ml-32m (test split, seed 42; paired bootstrap 1,000 resamples over 192,632 users)
Hyperparameters were chosen on the **validation** split only (16 configurations); each final config was
evaluated on test once.

| Config | NDCG@10 [95% CI] | Recall@50 | Coverage@10 | Δ NDCG@10 vs pop_count | Δ vs pop (damped) |
|---|---|---|---|---|---|
| Spec default: normalize, all neighbours, M=100, λ=100 | 0.0039 [0.0038, 0.0041] | 0.0169 | 0.2110 | −0.0400 [−0.0406, −0.0395] | — |
| Tuned: unnormalized, positive only, M=50, λ=30,000 (`configs/knn.ml-32m.tuned.json`) | **0.0726** [0.0719, 0.0733] | 0.2130 | 0.0419 | **+0.0286 [+0.0279, +0.0292]** | +0.0624 [+0.0618, +0.0631] |
| pop_count (baseline) | 0.0440 [0.0435, 0.0445] | 0.1565 | 0.0051 | — | — |
| pop, damped m = 10 (baseline) | 0.0101 [0.0099, 0.0103] | 0.0611 | 0.0008 | — | — |

### Validation search (ml-32m, validation split, NDCG@10)
| normalize | positive_only | M | λ | NDCG@10 | Coverage@10 |
|---|---|---|---|---|---|
| true | false | 100 | 100 | 0.0041 | 0.208 |
| true | true | 100 | 100 | 0.0060 | 0.194 |
| false | false | 100 | 100 | 0.0121 | 0.131 |
| false | true | 100 | 100 | 0.0309 | 0.106 |
| false | true | 20 / 50 / 300 | 25 | 0.0218 / 0.0180 / 0.0133 | ~0.19–0.20 |
| false | true | 20 / 50 / 300 | 100 | 0.0382 / 0.0334 / 0.0298 | ~0.10–0.12 |
| false | true | 10 / 20 | 300 | 0.0474 / 0.0470 | ~0.08 |
| false | true | 5 / 10 / 20 / 50 | 1,000 | 0.0541 / 0.0564 / 0.0573 / 0.0588 | ~0.06–0.08 |
| false | true | 20 | 3,000 / 10,000 / 30,000 / 100,000 | 0.0653 / 0.0701 / 0.0719 / 0.0724 | ~0.06 |
| false | true | **50** | **30,000** | **0.0735** | 0.049 |
| baseline pop_count | | | | 0.0466 | — |
| baseline pop (m = 10) | | | | 0.0097 | — |

## Findings
1. **The spec's KNN score is a rating predictor, and as a top-N ranker it is far worse than popularity.** Dividing
   by Σ|s'| makes an obscure item with one highly rated neighbour score like a confident recommendation.
   Removing the denominator and negative neighbours fixes most of it.
2. **Very large shrinkage wins.** With λ = 30,000 the similarity is dominated by co-rating counts, so the tuned
   model is close to a "people who rated X also rated Y" co-occurrence ranker. It is far more accurate than
   popularity but recommends a narrower, more mainstream set (coverage 4% vs 21% for the spec default; mean
   popularity rank 116 places more popular than pop_count's picks). Phase 6's MMR re-ranker targets exactly this.
3. **Content-based conflicts with its own gate.** Centering the profile on the user's mean cancels any feature
   shared by everything the user rated, so "rated only comedies" carries no comedy signal. Content NDCG@10 on
   ml-latest-small is 0.0025 (vs 0.0457 for pop_count); an ml-32m evaluation is pending the owner's decision.

## Owner decisions needed before sign-off
- **KNN:** (a) keep the spec formula as `knn` and report it as failing popularity; (b) amend the spec so `knn` is
  the unnormalized, positive-neighbour top-N score with validation-tuned M/λ (passes); or (c) ship both as
  separate models (e.g. `knn` and `knn_pred`).
- **Content:** (a) accept and document the limitation; (b) change the profile centering (e.g. damped or global
  mean); or (c) restate the gate.
