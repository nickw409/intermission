// Explicit low-rank synthetic ratings for the MF-SGD recovery gate (spec: Phase 3).
//
// Model of the data
//   p_u, q_i in R^r with every entry drawn i.i.d. from N(0, 1/sqrt(r)) (variance 1/sqrt(r)), so
//     Var(p_u . q_i) = r * (1/sqrt(r)) * (1/sqrt(r)) = 1
//   whatever the rank: the signal always has unit variance and `noise_sd` is directly comparable
//   to it.
//   r_ui = p_u . q_i + noise_sd * e_ui, e_ui ~ N(0, 1).
//   Each (u, i) is observed independently with probability `density`.
//   Values are real-valued and not clipped to the star scale; there is no global mean or bias, so
//   a biased MF model can represent the truth exactly with mu = b_u = b_i = 0.
//
// Held-out set
//   For each user, a seeded shuffle of their observations puts round(heldout_fraction * n) of them
//   in `heldout` (sorted by item), but a user with at least one observation always keeps one in
//   training. This is a random split, not temporal; timestamps are synthetic (u * n_items + i).
//
// Determinism: one Rng(seed) draws P, then Q, then the observation mask and noise in (u, i) order;
// each user's held-out shuffle uses Rng(mix_seed(seed, u, 1)). Only recs::Rng is used, so the
// output is identical across standard libraries.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "recs/dataset.hpp"
#include "recs/rng.hpp"
#include "recs/synthetic.hpp"

namespace recs::synthetic {

ExplicitLowRank low_rank_explicit(std::uint32_t n_users, std::uint32_t n_items, std::uint32_t rank,
                                  double noise_sd, double density, double heldout_fraction,
                                  std::uint64_t seed) {
  if (rank == 0) throw std::invalid_argument("low_rank_explicit: rank must be positive");
  if (!(noise_sd >= 0.0)) throw std::invalid_argument("low_rank_explicit: noise_sd must be >= 0");
  if (!(density >= 0.0 && density <= 1.0))
    throw std::invalid_argument("low_rank_explicit: density must be in [0, 1]");
  if (!(heldout_fraction >= 0.0 && heldout_fraction <= 1.0))
    throw std::invalid_argument("low_rank_explicit: heldout_fraction must be in [0, 1]");

  Rng rng(seed);
  const double factor_sd = std::pow(static_cast<double>(rank), -0.25);  // sqrt(1/sqrt(r))
  std::vector<double> P(std::size_t{n_users} * rank), Q(std::size_t{n_items} * rank);
  for (auto& x : P) x = factor_sd * rng.normal();
  for (auto& x : Q) x = factor_sd * rng.normal();

  ExplicitLowRank out;
  out.rank = rank;
  out.noise_sd = noise_sd;
  out.heldout.resize(n_users);

  std::vector<Triplet> train;
  std::vector<Rating> observed;
  for (std::uint32_t u = 0; u < n_users; ++u) {
    observed.clear();
    for (std::uint32_t i = 0; i < n_items; ++i) {
      if (rng.uniform01() >= density) continue;
      double v = 0.0;
      for (std::uint32_t f = 0; f < rank; ++f) v += P[std::size_t{u} * rank + f] * Q[std::size_t{i} * rank + f];
      v += noise_sd * rng.normal();
      const auto ts = static_cast<std::int64_t>(std::uint64_t{u} * n_items + i);
      observed.push_back({i, static_cast<float>(v), ts});
    }

    Rng split_rng(mix_seed(seed, u, 1));
    split_rng.shuffle(observed.begin(), observed.end());
    auto n_held = static_cast<std::size_t>(
        std::llround(heldout_fraction * static_cast<double>(observed.size())));
    if (!observed.empty() && n_held >= observed.size()) n_held = observed.size() - 1;

    for (std::size_t j = 0; j < observed.size(); ++j) {
      const Rating& r = observed[j];
      if (j < n_held)
        out.heldout[u].push_back(r);
      else
        train.push_back({u, r.item, r.value, r.ts});
    }
    std::sort(out.heldout[u].begin(), out.heldout[u].end(),
              [](const Rating& a, const Rating& b) { return a.item < b.item; });
  }

  out.train = make_dataset("synthetic_low_rank", n_users, n_items, std::move(train));
  return out;
}

}  // namespace recs::synthetic
