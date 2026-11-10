#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "recs/rng.hpp"

TEST_CASE("Rng is deterministic for a seed and differs across seeds", "[spine][rng]") {
  recs::Rng a(7), b(7), c(8);
  for (int i = 0; i < 100; ++i) {
    const auto x = a.next_u64();
    CHECK(x == b.next_u64());
    (void)c.next_u64();
  }
  CHECK(recs::Rng(7).next_u64() != recs::Rng(8).next_u64());
}

TEST_CASE("Rng::uniform stays in range and is roughly uniform", "[spine][rng]") {
  recs::Rng r(1);
  std::vector<int> counts(10, 0);
  for (int i = 0; i < 100000; ++i) {
    const auto v = r.uniform(10);
    REQUIRE(v < 10);
    ++counts[v];
  }
  for (int c : counts) CHECK(std::abs(c - 10000) < 500);
}

TEST_CASE("Rng::normal has mean ~0 and sd ~1", "[spine][rng]") {
  recs::Rng r(3);
  double s = 0, s2 = 0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double x = r.normal();
    s += x;
    s2 += x * x;
  }
  CHECK(std::abs(s / n) < 0.01);
  CHECK(std::abs(std::sqrt(s2 / n) - 1.0) < 0.01);
}

TEST_CASE("Rng::shuffle is a permutation", "[spine][rng]") {
  recs::Rng r(5);
  std::vector<int> v(100);
  for (int i = 0; i < 100; ++i) v[i] = i;
  r.shuffle(v.begin(), v.end());
  auto sorted = v;
  std::sort(sorted.begin(), sorted.end());
  for (int i = 0; i < 100; ++i) CHECK(sorted[i] == i);
}
