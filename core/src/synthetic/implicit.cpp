// Synthetic implicit-feedback data with a planted low-rank preference structure, for the ALS
// recovery gate (spec Phase 4: "on implicit data generated from rank-5 factors, AUC above 0.9").
//
// Generative model
//   1. Latent factors p_u, q_i in R^rank with i.i.d. N(0, 1/sqrt(rank)) entries, so the affinity
//      s_ui = p_u . q_i has variance rank * (1/sqrt(rank))^2 = 1 (same scaling as low_rank_explicit).
//   2. Every user consumes exactly n_pos = max(2, round(density * n_items)) distinct items, drawn
//      without replacement with probability proportional to exp(s_ui / T) (a softmax / Plackett-Luce
//      draw). This is done with the Gumbel-top-k trick: take the n_pos items with the largest
//      s_ui / T + g_ui, g_ui ~ Gumbel(0, 1) (Kool, van Hoof & Welling, ICML 2019).
//      The temperature T = kTemperature = 0.25 is fixed: low enough that the planted structure is
//      clearly present, high enough that positives are not simply each user's top items.
//      How much structure there is to recover: ranking held-out positives by the TRUE affinity
//      s_ui (the best any model can do) gives a mean per-user AUC of about 0.96 at T = 0.25
//      (1000 users, 400 items, rank 5, density 0.05), about 0.93 at T = 0.35 and only about 0.88
//      at T = 0.5, where no model could pass the spec's AUC > 0.9 gate.
//   3. n_held = round(heldout_fraction * n_pos), clamped so at least one positive stays in
//      training, of each user's positives (a seeded uniform choice) go to heldout_positive; the
//      rest become training ratings with value 5.0 (so they pass any interaction threshold) and
//      timestamp 0.
// Everything is drawn from recs::Rng (portable), so the data is identical for a given seed on
// every platform.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#include "recs/dataset.hpp"
#include "recs/rng.hpp"
#include "recs/synthetic.hpp"

namespace recs::synthetic {

namespace {
constexpr double kTemperature = 0.25;
}  // namespace

ImplicitPlanted planted_implicit(std::uint32_t n_users, std::uint32_t n_items, std::uint32_t rank,
                                 double density, double heldout_fraction, std::uint64_t seed) {
  if (n_users == 0 || n_items < 2 || rank == 0) throw std::invalid_argument("planted_implicit: empty problem");
  if (!(density > 0.0 && density <= 1.0)) throw std::invalid_argument("planted_implicit: density must be in (0, 1]");
  if (!(heldout_fraction >= 0.0 && heldout_fraction < 1.0))
    throw std::invalid_argument("planted_implicit: heldout_fraction must be in [0, 1)");

  Rng rng(seed);
  const double sd = 1.0 / std::sqrt(std::sqrt(static_cast<double>(rank)));
  std::vector<double> P(static_cast<std::size_t>(n_users) * rank);
  std::vector<double> Q(static_cast<std::size_t>(n_items) * rank);
  for (auto& v : P) v = sd * rng.normal();
  for (auto& v : Q) v = sd * rng.normal();

  const auto n_pos = static_cast<std::uint32_t>(
      std::clamp<double>(std::round(density * n_items), 2.0, static_cast<double>(n_items)));
  const auto n_held = static_cast<std::uint32_t>(
      std::min<double>(std::round(heldout_fraction * n_pos), static_cast<double>(n_pos - 1)));

  ImplicitPlanted out;
  out.rank = rank;
  out.heldout_positive.resize(n_users);
  std::vector<Triplet> train;
  train.reserve(static_cast<std::size_t>(n_users) * (n_pos - n_held));

  std::vector<std::pair<double, ItemIdx>> keys(n_items);
  for (std::uint32_t u = 0; u < n_users; ++u) {
    for (ItemIdx i = 0; i < n_items; ++i) {
      double s = 0.0;
      for (std::uint32_t a = 0; a < rank; ++a) s += P[u * rank + a] * Q[i * rank + a];
      double U = rng.uniform01();
      while (U <= 0.0) U = rng.uniform01();
      const double gumbel = -std::log(-std::log(U));
      keys[i] = {s / kTemperature + gumbel, i};
    }
    // Top n_pos keys (ties by lower item, for determinism).
    std::partial_sort(keys.begin(), keys.begin() + n_pos, keys.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<ItemIdx> chosen(n_pos);
    for (std::uint32_t j = 0; j < n_pos; ++j) chosen[j] = keys[j].second;
    std::sort(chosen.begin(), chosen.end());
    rng.shuffle(chosen.begin(), chosen.end());
    std::sort(chosen.begin(), chosen.begin() + n_held);
    std::sort(chosen.begin() + n_held, chosen.end());
    for (std::uint32_t j = 0; j < n_held; ++j) out.heldout_positive[u].push_back({chosen[j], 5.0f, 0});
    for (std::uint32_t j = n_held; j < n_pos; ++j) train.push_back({u, chosen[j], 5.0f, 0});
  }
  out.train = make_dataset("planted_implicit", n_users, n_items, std::move(train));
  return out;
}

}  // namespace recs::synthetic
