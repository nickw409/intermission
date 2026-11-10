// Portable, seeded RNG. Use this everywhere instead of <random> distributions, whose output is
// implementation-defined and would break byte-identical reports across standard libraries.
// FROZEN after Wave 0.
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace recs {

__extension__ using u128 = unsigned __int128;  // GCC/Clang extension; __extension__ silences -Wpedantic

/// SplitMix64 step: also used to derive independent seeds (e.g. per user, per thread).
constexpr std::uint64_t splitmix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// Stateless hash of (seed, a, b): deterministic tie-breaking keys and per-entity seeds.
constexpr std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t a, std::uint64_t b = 0) {
  std::uint64_t s = seed ^ (a * 0xD6E8FEB86659FD93ull) ^ (b * 0xA0761D6478BD642Full);
  return splitmix64(s);
}

/// xoshiro256** seeded through SplitMix64.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    std::uint64_t s = seed;
    for (auto& w : s_) w = splitmix64(s);
  }
  std::uint64_t next_u64() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t; s_[3] = rotl(s_[3], 45);
    return result;
  }
  /// Uniform integer in [0, bound), unbiased (Lemire's method). bound must be > 0.
  std::uint64_t uniform(std::uint64_t bound) {
    u128 m = static_cast<u128>(next_u64()) * bound;
    auto low = static_cast<std::uint64_t>(m);
    if (low < bound) {
      const std::uint64_t threshold = (0 - bound) % bound;
      while (low < threshold) {
        m = static_cast<u128>(next_u64()) * bound;
        low = static_cast<std::uint64_t>(m);
      }
    }
    return static_cast<std::uint64_t>(m >> 64);
  }
  /// Uniform double in [0, 1) with 53 random bits.
  double uniform01() { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }
  /// Standard normal via Box-Muller (no cached second value, so the stream is simple to reason about).
  double normal() {
    double u1 = uniform01();
    while (u1 <= 0.0) u1 = uniform01();
    const double u2 = uniform01();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
  }
  /// Fisher-Yates shuffle.
  template <class It>
  void shuffle(It first, It last) {
    const auto n = static_cast<std::uint64_t>(last - first);
    for (std::uint64_t i = n; i > 1; --i) {
      const std::uint64_t j = uniform(i);
      using std::swap;
      swap(first[static_cast<std::ptrdiff_t>(i - 1)], first[static_cast<std::ptrdiff_t>(j)]);
    }
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  std::uint64_t s_[4]{};
};

}  // namespace recs
