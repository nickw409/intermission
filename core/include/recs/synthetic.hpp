// Synthetic datasets for model tests. FROZEN after Wave 0.
// low_rank_explicit is implemented by workstream F (core/src/synthetic/low_rank.cpp);
// planted_implicit by workstream G (core/src/synthetic/implicit.cpp).
#pragma once

#include <cstdint>
#include <vector>

#include "recs/types.hpp"

namespace recs::synthetic {

struct ExplicitLowRank {
  Dataset train;
  std::vector<std::vector<Rating>> heldout;  // indexed by UserIdx
  std::uint32_t rank = 0;
  double noise_sd = 0.0;
};

/// r_ui = p_u . q_i + N(0, noise_sd^2) with p, q ~ N(0, 1/sqrt(rank)); each (u, i) observed with
/// probability `density`; a `heldout_fraction` of each user's observations is held out.
/// Values are real-valued (not rounded to 0.5 steps).
ExplicitLowRank low_rank_explicit(std::uint32_t n_users, std::uint32_t n_items, std::uint32_t rank,
                                  double noise_sd, double density, double heldout_fraction,
                                  std::uint64_t seed);

struct ImplicitPlanted {
  Dataset train;                                      // positives stored as rating 5.0
  std::vector<std::vector<Rating>> heldout_positive;  // indexed by UserIdx
  std::uint32_t rank = 0;
};

/// Positives drawn from a planted rank-`rank` preference structure; documented in implicit.cpp.
ImplicitPlanted planted_implicit(std::uint32_t n_users, std::uint32_t n_items, std::uint32_t rank,
                                 double density, double heldout_fraction, std::uint64_t seed);

}  // namespace recs::synthetic
