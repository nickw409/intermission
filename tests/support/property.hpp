// Seeded property-test helper. Every failure message includes the seed so it can be replayed.
#pragma once

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "recs/rng.hpp"

namespace recs::testing {

/// Runs fn(rng, seed) for `cases` seeds derived from `base_seed`.
/// Use INFO-captured seeds: a failing case prints "seed = N"; replay with for_seeds(1, N, fn).
template <class Fn>
void for_seeds(int cases, std::uint64_t base_seed, Fn&& fn) {
  for (int i = 0; i < cases; ++i) {
    const std::uint64_t seed = (cases == 1) ? base_seed : mix_seed(base_seed, static_cast<std::uint64_t>(i));
    INFO("seed = " << seed);
    Rng rng(seed);
    fn(rng, seed);
  }
}

}  // namespace recs::testing
