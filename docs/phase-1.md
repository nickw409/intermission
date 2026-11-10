# Phase 1 — Item-KNN and content-based

**Status: NOT signed off. Owner decisions made 2026-11-10; implementation in progress.**
- **KNN: option (b).** The spec now defines `knn` as the top-N score: no Σ|sim| denominator, positive neighbours only, and M and λ chosen on validation. The rating-prediction form stays as a config option for comparison.
- **Content: option (b).** Change the profile centering so that a feature shared by everything a user rated still counts. The exact centering is chosen on validation and must pass the honest one-genre gate. The spec text follows once it is chosen.
- **MF (Phase 3, raised early):** make SGD training converge, so that trained user vectors match fold-in (parity > 0.9 on ml-32m). Do this with hyperparameters chosen on validation; no gate changes.

The numbers below are the pre-decision results.

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
   popularity but recommends a narrower set than the spec default (coverage 4% vs 21%). Its picks are still
   less popular than pop_count's (mean popularity rank 116 places lower), but it covers far less of the catalog
   than the spec formula did. Phase 6's MMR re-ranker targets exactly this.
3. **Content-based conflicts with its own gate.** Centering the profile on the user's mean cancels any feature
   shared by everything the user rated, so "rated only comedies" carries no comedy signal. Content NDCG@10 on
   ml-latest-small is 0.0025 (vs 0.0457 for pop_count); an ml-32m evaluation is pending the owner's decision.

## Owner decisions needed before sign-off
- **KNN:** (a) keep the spec formula as `knn` and report it as failing popularity; (b) amend the spec so `knn` is
  the unnormalized, positive-neighbour top-N score with validation-tuned M/λ (passes); or (c) ship both as
  separate models (e.g. `knn` and `knn_pred`).
- **Content:** (a) accept and document the limitation; (b) change the profile centering (e.g. damped or global
  mean); or (c) restate the gate.

## Content centering (option b)
Implemented on branch `phase1/content-centering` (commits `eec6ba0`, `30a9e35`, `af6eaf7` and the
review fixes after them). The profile weight is now the damped baseline-predictor residual
w_uj = r_uj − (μ + b_u + b_j) (Koren 2010, §2.1), with β_u = 100 and β_i = 1,000. The math and the reason
the user-mean centering fails are in the comment at the top of `core/src/models/content.cpp`.

### One-genre gate fixture (owner-approved change)
With the original fixture, 2 of 20 seeds drew only 14 comedies. The user rates 8, so only 6 stayed
unrated and "≥ 7 of 10 in-genre" was impossible for any model. The owner approved fixing the
fixture and keeping the threshold. Catalogs with fewer than 15 comedies are now redrawn from the
same seeded stream, and the preconditions require ≥ 15 comedies and ≥ 7 unrated comedy candidates.
Only those 2 seeds changed (each now has 11 unrated comedies). The gate column below is from the
hidden test `recs_tests "[.centering-comparison]" -s` on the new fixture.

### Validation search (ml-latest-small, validation split, split seed 42, 610 users)
Every row is one run of

```bash
build/release/train/recs-train eval --dataset ml-latest-small --model content --on validation --config <file>
```

where `<file>` holds the JSON in the first column (other keys default: `min_tag_count` 2,
`genre_weight` 1.0). The runs used the implementation later committed as `30a9e35`, before it was
committed (so the reports say `git_sha: unknown`). The midpoint row used a temporary option
(r − 2.75) that was not committed; it was a comparison baseline only.

| Config | NDCG@10 [95% CI] | Recall@50 | Coverage@10 | Gate (seeds ≥ 7/10) | Run id |
|---|---|---|---|---|---|
| `{"centering":"user_mean"}` | 0.0023 [0.0008, 0.0042] | 0.0119 | 0.2057 | 0/20 | `2026-11-10T20-31-06Z_content` |
| `{"centering":"global_mean"}` | 0.0038 [0.0018, 0.0058] | 0.0142 | 0.1785 | 20/20 | `2026-11-10T20-31-02Z_content` |
| `{"centering":"damped_user_mean","user_damping":1}` | 0.0023 [0.0008, 0.0042] | 0.0125 | 0.2037 | 5/20 | `2026-11-10T20-30-52Z_content` |
| `{"centering":"damped_user_mean","user_damping":3}` | 0.0025 [0.0009, 0.0044] | 0.0128 | 0.1998 | 15/20 | `2026-11-10T20-30-57Z_content` |
| `{"centering":"damped_user_mean","user_damping":10}` | 0.0032 [0.0015, 0.0054] | 0.0122 | 0.1987 | 20/20 | `2026-11-10T20-30-53Z_content` |
| `{"centering":"damped_user_mean","user_damping":30}` | 0.0039 [0.0019, 0.0061] | 0.0143 | 0.1958 | 20/20 | `2026-11-10T20-30-59Z_content` |
| `{"centering":"damped_user_mean","user_damping":100}` | 0.0049 [0.0024, 0.0082] | 0.0148 | 0.1875 | 20/20 | `2026-11-10T20-30-55Z_content` |
| `{"centering":"damped_user_mean","user_damping":300}` | 0.0044 [0.0023, 0.0068] | 0.0148 | 0.1838 | 20/20 | `2026-11-10T20-31-00Z_content` |
| `{"centering":"damped_user_mean","user_damping":1000}` | 0.0042 [0.0021, 0.0067] | 0.0153 | 0.1825 | 20/20 | `2026-11-10T20-31-43Z_content` |
| `{"centering":"damped_user_mean","user_damping":3000}` | 0.0038 [0.0019, 0.0059] | 0.0142 | 0.1793 | 20/20 | `2026-11-10T20-31-44Z_content` |
| `{"centering":"baseline","user_damping":3,"item_damping":10}` | 0.0029 [0.0010, 0.0053] | 0.0168 | 0.2301 | 13/20 | `2026-11-10T20-30-46Z_content` |
| `{"centering":"baseline","user_damping":3,"item_damping":25}` | 0.0026 [0.0008, 0.0050] | 0.0141 | 0.2205 | 16/20 | `2026-11-10T20-30-50Z_content` |
| `{"centering":"baseline","user_damping":3,"item_damping":100}` | 0.0027 [0.0008, 0.0051] | 0.0119 | 0.2136 | 16/20 | `2026-11-10T20-30-48Z_content` |
| `{"centering":"baseline","user_damping":10,"item_damping":10}` | 0.0031 [0.0009, 0.0062] | 0.0167 | 0.2219 | 20/20 | `2026-11-10T20-30-35Z_content` |
| `{"centering":"baseline","user_damping":10,"item_damping":25}` | 0.0030 [0.0012, 0.0053] | 0.0159 | 0.2159 | 20/20 | `2026-11-10T20-30-39Z_content` |
| `{"centering":"baseline","user_damping":10,"item_damping":100}` | 0.0032 [0.0013, 0.0059] | 0.0140 | 0.2119 | 20/20 | `2026-11-10T20-30-37Z_content` |
| `{"centering":"baseline","user_damping":30,"item_damping":10}` | 0.0033 [0.0012, 0.0060] | 0.0141 | 0.2173 | 20/20 | `2026-11-10T20-30-41Z_content` |
| `{"centering":"baseline","user_damping":30,"item_damping":25}` | 0.0038 [0.0014, 0.0071] | 0.0146 | 0.2103 | 20/20 | `2026-11-10T20-30-44Z_content` |
| `{"centering":"baseline","user_damping":30,"item_damping":100}` | 0.0036 [0.0015, 0.0062] | 0.0157 | 0.2027 | 20/20 | `2026-11-10T20-30-43Z_content` |
| `{"centering":"baseline","user_damping":100,"item_damping":10}` | 0.0029 [0.0010, 0.0053] | 0.0135 | 0.2093 | 20/20 | `2026-11-10T20-30-30Z_content` |
| `{"centering":"baseline","user_damping":100,"item_damping":25}` | 0.0036 [0.0012, 0.0068] | 0.0149 | 0.2015 | 20/20 | `2026-11-10T20-30-34Z_content` |
| `{"centering":"baseline","user_damping":100,"item_damping":100}` | 0.0051 [0.0023, 0.0082] | 0.0141 | 0.1990 | 20/20 | `2026-11-10T20-30-32Z_content` |
| `{"centering":"baseline","user_damping":100,"item_damping":300}` | 0.0053 [0.0026, 0.0082] | 0.0152 | 0.1933 | 20/20 | `2026-11-10T20-31-32Z_content` |
| **`{"centering":"baseline","user_damping":100,"item_damping":1000}`** | 0.0058 [0.0031, 0.0091] | 0.0154 | 0.1885 | 20/20 | `2026-11-10T20-31-30Z_content` |
| `{"centering":"baseline","user_damping":100,"item_damping":3000}` | 0.0048 [0.0023, 0.0080] | 0.0148 | 0.1898 | 20/20 | `2026-11-10T20-31-34Z_content` |
| `{"centering":"baseline","user_damping":300,"item_damping":100}` | 0.0042 [0.0018, 0.0068] | 0.0147 | 0.1966 | 20/20 | `2026-11-10T20-31-36Z_content` |
| `{"centering":"baseline","user_damping":300,"item_damping":300}` | 0.0044 [0.0022, 0.0068] | 0.0142 | 0.1903 | 20/20 | `2026-11-10T20-31-39Z_content` |
| `{"centering":"baseline","user_damping":300,"item_damping":1000}` | 0.0049 [0.0024, 0.0074] | 0.0150 | 0.1849 | 20/20 | `2026-11-10T20-31-37Z_content` |
| `{"centering":"baseline","user_damping":300,"item_damping":3000}` | 0.0048 [0.0025, 0.0073] | 0.0152 | 0.1861 | 20/20 | `2026-11-10T20-31-41Z_content` |
| `{"centering":"baseline","user_damping":1000,"item_damping":100}` | 0.0045 [0.0021, 0.0073] | 0.0151 | 0.1916 | 20/20 | `2026-11-10T20-31-23Z_content` |
| `{"centering":"baseline","user_damping":1000,"item_damping":300}` | 0.0045 [0.0023, 0.0070] | 0.0145 | 0.1868 | 20/20 | `2026-11-10T20-31-26Z_content` |
| `{"centering":"baseline","user_damping":1000,"item_damping":1000}` | 0.0045 [0.0023, 0.0070] | 0.0157 | 0.1842 | 20/20 | `2026-11-10T20-31-24Z_content` |
| `{"centering":"baseline","user_damping":1000,"item_damping":3000}` | 0.0048 [0.0025, 0.0074] | 0.0157 | 0.1847 | 20/20 | `2026-11-10T20-31-28Z_content` |
| `{"centering":"midpoint"}` | 0.0027 [0.0008, 0.0052] | 0.0146 | 0.1226 | old fixture: 18/20 (the 2 misses were the infeasible seeds); not committed | `2026-11-10T20-31-04Z_content` |

**Choice.** Among the configurations that pass the gate on all 20 seeds, baseline with β_u = 100 and
β_i = 1,000 has the highest validation NDCG@10. Coverage@10 was a secondary criterion and did not
change the choice: it is 0.18–0.23 for every passing configuration.

**Caveat (selection optimism).** This is the best of about 35 configurations on 575 users with a
relevant item, and the CIs of nearly all non-user-mean rows overlap. Expect its validation number
to be optimistic. Paired bootstraps on validation (`recs-train compare`, 1,000 resamples, seed 42,
from `--keep-per-user` reruns of four configs: `2026-11-10T20-31-59Z_content` = chosen,
`20-32-01Z` = damped β 100, `20-32-03Z` = global_mean, `20-32-05Z` = user_mean):

| Chosen minus | Δ NDCG@10 [95% CI] |
|---|---|
| global_mean | +0.0021 [+0.0003, +0.0045] |
| user_mean | +0.0035 [+0.0014, +0.0059] |
| damped_user_mean β 100 | +0.0009 [−0.0001, +0.0021] (not significant) |

### Test split (final config `configs/content.json`, split seed 42, 1,000 bootstrap resamples, 2 OpenMP threads)
Each row ran once:

```bash
build/release/train/recs-train eval --dataset <ds> --model <m> --config configs/<m>.json --on test --keep-per-user
```

ml-32m runs were wrapped in `flock data/.ml32m.lock`. Wall time and peak RSS come from `/usr/bin/time -v`.

| Dataset | Model | NDCG@10 [95% CI] | Recall@50 | Coverage@10 | Wall time | Peak RSS | Run id |
|---|---|---|---|---|---|---|---|
| ml-latest-small | content (baseline centering) | 0.0021 [0.0009, 0.0035] | 0.0097 | 0.190 | 0.27 s | 25 MB | `2026-11-10T20-38-18Z_content` |
| ml-latest-small | content, user_mean (old) | 0.0025 [0.0006, 0.0052] | 0.0109 | 0.200 | — | — | `2026-11-10T20-38-42Z_content` |
| ml-latest-small | pop | 0.0355 [0.0281, 0.0444] | 0.1146 | 0.0068 | 0.12 s | 25 MB | `2026-11-10T20-38-19Z_pop` |
| ml-latest-small | pop_count | 0.0457 [0.0369, 0.0558] | 0.1513 | 0.0123 | 0.11 s | 25 MB | `2026-11-10T20-38-21Z_pop_count` |
| ml-32m | content (baseline centering) | 0.0006 [0.0005, 0.0007] | 0.0025 | 0.190 | 6 min 16 s (fit 2.4 s, eval 353 s) | 3.77 GB | `2026-11-10T20-43-48Z_content` |
| ml-32m | pop | 0.0101 [0.0099, 0.0103] | 0.0611 | 0.0008 | 1 min 22 s | 3.53 GB | `2026-11-10T21-09-00Z_pop` |
| ml-32m | pop_count | 0.0440 [0.0435, 0.0445] | 0.1565 | 0.0051 | 1 min 27 s | 3.53 GB | `2026-11-10T21-54-50Z_pop_count` |

Paired deltas (content minus the other run; `recs-train compare`, 1,000 resamples, seed 42):

| Dataset | vs | Users | Δ NDCG@10 [95% CI] | Δ Recall@50 [95% CI] |
|---|---|---|---|---|
| ml-latest-small | pop_count | 570 | −0.0436 [−0.0537, −0.0346] | −0.1417 [−0.1614, −0.1223] |
| ml-latest-small | pop | 570 | −0.0334 [−0.0423, −0.0255] | −0.1050 [−0.1245, −0.0876] |
| ml-latest-small | content, user_mean (old) | 570 | −0.0003 [−0.0032, +0.0017] (not significant) | −0.0012 [−0.0074, +0.0040] |
| ml-32m | pop_count | 192,632 | −0.0434 [−0.0439, −0.0429] | −0.1541 [−0.1552, −0.1530] |
| ml-32m | pop | 192,632 | −0.0095 [−0.0097, −0.0093] | −0.0587 [−0.0593, −0.0580] |

The old centering was not evaluated on the ml-32m test split.

**Reading.** The new centering fixes the one-genre gate: 20/20 seeds against 0/20 for the user mean.
On ranking accuracy, content is far below both popularity baselines on both datasets. On the
ml-latest-small test split it is indistinguishable from the old centering, although validation
favoured it. Content has no "beat popularity" gate (only KNN does).

**Deploy note.** Content artifacts now include `content/centering.bin`. An artifact saved before
this change is refused with "content artifact predates centering.bin; retrain"
(`deploy/runbooks/deploy.md`).
