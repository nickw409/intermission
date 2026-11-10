# G — Implicit ALS (phase 4, started early)

**Owns:** `core/src/models/als.cpp` (+ optional `als_detail.hpp`), `core/src/synthetic/implicit.cpp`,
`tests/core/models/als/`
**Implements:** `make_als()`, a `Model` named `"als"`, and `synthetic::planted_implicit` (frozen `recs/model.hpp`,
`recs/synthetic.hpp`, `recs/foldin.hpp` `solve_spd`, `recs/metrics.hpp` `roc_auc`). Protocol: `docs/plans/wave-1/README.md`.
**Spec:** "Models → Implicit ALS", "Numerics and determinism", Phase 4 gates.

## Math (comment block with references at the top of als.cpp — Hu, Koren, Volinsky 2008; Takács et al. 2011)
- Interaction p_ui = 1 iff rating ≥ `interaction_threshold` (3.5); confidence c_ui = 1 + α·p_ui (α = `alpha`, 40).
- Alternate closed-form solves: x_u = (YᵀY + Yᵀ(Cᵘ − I)Y + λI)⁻¹ YᵀCᵘp(u), using the YᵀY trick so each
  solve touches only the user's interactions; same for items with X fixed. Cholesky in double (`solve_spd`),
  factors stored as float32. Initialize factors N(0, init_sd²) with `recs::Rng(seed)`.
- OpenMP over users/items with a static schedule; each solve independent → deterministic.
- Objective (for the curve and gate 1): Σ_u,i c_ui (p_ui − x_uᵀy_i)² + λ(Σ‖x_u‖² + Σ‖y_i‖²), computed in double
  with the same trick (no dense |U|×|I| loop). training_curve = (iteration, objective), metric "objective".
- fold_in(ratings) = exactly the user solve with Y fixed; score = x_uᵀy_i. Batch path: trained x_u.
- explain(): rated (interacted) items j with the largest contribution to x_uᵀy_i under the closed form
  (document the decomposition you use; e.g. the item-based view of the ALS solution from Hu et al. §5); top 3,
  positive, only rated items.
- save/load: binary Y (items × k float32) + config; X is not needed for serving (don't save it); header,
  hostile-input safe.

## Gate tests (write first)
1. Objective non-increasing after EVERY half-step (user solve, item solve) within relative tolerance 1e-9.
2. Each per-user solve matches a direct dense solve of the same system (build the full k×k system densely
   without the YᵀY trick, solve in double) to 1e-6.
3. Synthetic recovery: implicit data from planted rank-5 factors (`planted_implicit`, documented in implicit.cpp);
   held-out AUC > 0.9 (use `roc_auc`, excluding the user's training positives).
4. Fold-in parity on the synthetic data: Jaccard@10 batch vs fold_in > 0.95 (fast proxy for the ml-32m gate).
5. Determinism 1 vs 4 threads (bitwise factors); save/load round-trip bitwise scores; hostile files rejected.

At the end: fit time and peak RSS on ml-32m (release, OMP_NUM_THREADS=4, k = 64, 15 iterations) — the only
ml-32m job you run. Never loosen a spec threshold; if one seems unattainable for a principled reason, stop and report.
