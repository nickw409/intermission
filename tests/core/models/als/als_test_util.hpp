// Shared helpers for the ALS tests.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "recs/dataset.hpp"
#include "recs/rng.hpp"
#include "recs/types.hpp"

namespace recs::testing::als {

/// Random explicit ratings in 0.5 steps, each (u, i) present with probability `density`, so some
/// ratings fall below the interaction threshold and some users/items have no interactions.
inline Dataset random_ratings(std::uint32_t n_users, std::uint32_t n_items, double density, Rng& rng) {
  std::vector<Triplet> t;
  for (std::uint32_t u = 0; u < n_users; ++u)
    for (std::uint32_t i = 0; i < n_items; ++i)
      if (rng.uniform01() < density)
        t.push_back({u, i, 0.5f * static_cast<float>(1 + rng.uniform(10)), 0});
  return make_dataset("random", n_users, n_items, std::move(t));
}

/// Row-major n x k matrix with N(0, sd^2) entries.
inline std::vector<float> random_factors(std::size_t n, std::size_t k, double sd, Rng& rng) {
  std::vector<float> f(n * k);
  for (auto& x : f) x = static_cast<float>(sd * rng.normal());
  return f;
}

/// One user's training ratings as Rating values.
inline std::vector<Rating> user_ratings(const Dataset& ds, UserIdx u) {
  std::vector<Rating> r;
  const auto items = ds.by_user.row_indices(u);
  const auto values = ds.by_user.row_values(u);
  for (std::size_t j = 0; j < items.size(); ++j) r.push_back({items[j], values[j], 0});
  return r;
}

/// Top-k items by score (descending, ties by lower item), skipping `exclude` and -inf scores.
/// Written here so the tests do not depend on the shared recommend().
inline std::vector<ItemIdx> top_k(std::span<const float> scores, std::span<const Rating> exclude, std::size_t k) {
  std::vector<char> skip(scores.size(), 0);
  for (const auto& r : exclude) skip[r.item] = 1;
  std::vector<ItemIdx> items;
  for (ItemIdx i = 0; i < scores.size(); ++i)
    if (!skip[i] && scores[i] != -std::numeric_limits<float>::infinity()) items.push_back(i);
  std::sort(items.begin(), items.end(), [&](ItemIdx a, ItemIdx b) {
    return scores[a] != scores[b] ? scores[a] > scores[b] : a < b;
  });
  if (items.size() > k) items.resize(k);
  return items;
}

}  // namespace recs::testing::als
