# F — Matrix factorization with SGD (phase 3, started early)

**Owns:** `core/src/models/mf_sgd.cpp` (+ optional `mf_sgd_detail.hpp`), `core/src/synthetic/low_rank.cpp`,
`tests/core/models/mf_sgd/`
**Implements:** `make_mf_sgd()`, a `Model` named `"mf"`, and `synthetic::low_rank_explicit` (frozen
`recs/model.hpp`, `recs/synthetic.hpp`, `recs/foldin.hpp` `solve_spd`). Protocol: `docs/plans/wave-1/README.md`.
**Spec:** "Models → Matrix factorization (SGD)", "Numerics and determinism", Phase 3 gates.

## Math (comment block with references at the top of mf_sgd.cpp — Koren, Bell, Volinsky 2009; Funk 2006)
- r̂_ui = μ + b_u + b_i + p_uᵀq_i; loss Σ (r − r̂)² + λ(b_u² + b_i² + ‖p_u‖² + ‖q_i‖²).
- SGD: shuffle all training ratings every epoch with `recs::Rng(seed ^ epoch)`; learning rate `lr`, decayed by
  `lr_decay` each epoch; early stopping on validation RMSE (`ctx.validation`) with `patience`; keep the best
  epoch's parameters. Without validation, run `max_epochs`. Deterministic mode: single-threaded SGD in shuffled
  order (no Hogwild). Record `training_curve` = (epoch, validation RMSE) (or training loss without validation),
  `training_curve_metric` accordingly.
- `predicts_ratings() = true`; score() returns r̂ (μ + b_u + b_i + p_uᵀq_i). Items never seen in training:
  b_i = 0, q_i = 0 → score = μ + b_u (finite, so the rating task doesn't need the fallback).
- fold_in(ratings): b_u = damped mean residual Σ(r − μ − b_i)/(n + λ_b) with λ_b documented (use 10 to match
  the evaluator's rating baselines); then p_u = solve_spd(Qᵀ_R Q_R, Q_Rᵀ(r − μ − b_u − b_i), k, λ·n?) — choose
  and document the ridge scaling so that fold-in on a training user approximates its trained vector.
- Batch path: `supports_trained_scoring() = true`; `score_trained_user(u)` uses the trained b_u, p_u.
- explain(): rated items j with the largest q_jᵀq_i·(r_uj − predicted) style contribution — define it, document it,
  top 3 positive; only rated items.
- save/load: binary (μ, biases, P not saved for serving — only Q, b_i, μ, config); little-endian with header.

## Gate tests (write first)
1. Analytic gradient matches central finite differences in DOUBLE precision, relative error < 1e-5 for every
   parameter block (μ fixed; b_u, b_i, p_u, q_i) on a small random problem (expose a double-precision loss and
   gradient in mf_sgd_detail.hpp).
2. Synthetic recovery: `low_rank_explicit` rank 5, noise N(0, 0.1²); held-out RMSE < 0.15 (spec threshold —
   never loosen; tune lr/λ/epochs/size of the synthetic problem honestly and document).
3. Fold-in parity on the synthetic data: Jaccard@10 between batch path and fold-in > 0.9 (Phase 3 gate is on
   ml-32m; this is the fast proxy).
4. Determinism: identical parameters and curve for the same seed; differ for a different seed.
5. save/load round-trip bitwise identical scores; hostile files rejected.
