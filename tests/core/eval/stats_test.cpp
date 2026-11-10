// Gate tests for recs/stats.hpp (B1 gate 3): percentile bootstrap over users.
#include <cmath>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "recs/metrics.hpp"
#include "recs/rng.hpp"
#include "recs/stats.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<double> normal_sample(std::uint64_t seed, std::size_t n, double mean, double sd) {
  recs::Rng rng(seed);
  std::vector<double> x(n);
  for (auto& v : x) v = mean + sd * rng.normal();
  return x;
}

double sample_mean(const std::vector<double>& x) {
  double s = 0.0;
  for (double v : x) s += v;
  return s / static_cast<double>(x.size());
}

}  // namespace

TEST_CASE("bootstrap_ci of a constant collapses to the constant", "[eval][stats]") {
  // Every resample of a constant vector has the same mean, so every percentile is that constant.
  // 0.25 is exact in binary, so the sums are exact too.
  const std::vector<double> c(50, 0.25);
  const auto e = recs::bootstrap_ci(c);
  CHECK(e.mean == 0.25);
  CHECK(e.lo == 0.25);
  CHECK(e.hi == 0.25);
  // 0.1 is not exact in binary: summing 37 copies and dividing rounds, so allow 1e-15.
  const std::vector<double> d(37, 0.1);
  const auto f = recs::bootstrap_ci(d, 200, 7);
  CHECK_THAT(f.mean, WithinAbs(0.1, 1e-15));
  CHECK_THAT(f.lo, WithinAbs(0.1, 1e-15));
  CHECK_THAT(f.hi, WithinAbs(0.1, 1e-15));
}

TEST_CASE("bootstrap_ci mean is the sample mean and the CI brackets it", "[eval][stats]") {
  // {1, 2, 3, 4}: mean = 10 / 4 = 2.5. Resample means lie in [1, 4].
  const std::vector<double> x{1.0, 2.0, 3.0, 4.0};
  const auto e = recs::bootstrap_ci(x);
  CHECK(e.mean == 2.5);
  CHECK(e.lo >= 1.0);
  CHECK(e.hi <= 4.0);
  CHECK(e.lo < e.mean);
  CHECK(e.hi > e.mean);

  const auto y = normal_sample(11, 300, 1.0, 2.0);
  const auto ey = recs::bootstrap_ci(y);
  CHECK_THAT(ey.mean, WithinAbs(sample_mean(y), 1e-12));
}

TEST_CASE("bootstrap_ci is deterministic for a seed", "[eval][stats]") {
  const auto x = normal_sample(5, 100, 0.0, 1.0);
  const auto a = recs::bootstrap_ci(x, 1000, 42);
  const auto b = recs::bootstrap_ci(x, 1000, 42);
  CHECK(a.mean == b.mean);
  CHECK(a.lo == b.lo);
  CHECK(a.hi == b.hi);
  const auto c = recs::bootstrap_ci(x, 1000, 43);
  CHECK(c.mean == a.mean);            // the point estimate does not depend on the seed
  CHECK((c.lo != a.lo || c.hi != a.hi));  // the resamples do
}

TEST_CASE("bootstrap_ci width shrinks like 1/sqrt(n)", "[eval][stats]") {
  // For a normal sample with sd 1 the 95% CI width is about 2 * 1.96 / sqrt(n).
  // n -> 4n should halve the width: ratio 2. Sampling noise in the sd estimate (~3.5% at n = 400)
  // and in the bootstrap percentiles (~3% each) keeps the ratio within [1.7, 2.3].
  const auto small = normal_sample(21, 400, 0.0, 1.0);
  const auto large = normal_sample(22, 1600, 0.0, 1.0);
  const auto es = recs::bootstrap_ci(small);
  const auto el = recs::bootstrap_ci(large);
  const double ws = es.hi - es.lo;
  const double wl = el.hi - el.lo;
  INFO("width(n=400) = " << ws << ", width(n=1600) = " << wl);
  CHECK(ws / wl > 1.7);
  CHECK(ws / wl < 2.3);
  // Absolute sanity: 2 * 1.96 / sqrt(400) = 0.196.
  CHECK_THAT(ws, WithinRel(0.196, 0.15));
}

TEST_CASE("bootstrap_ci higher level gives a wider interval", "[eval][stats]") {
  const auto x = normal_sample(8, 200, 0.0, 1.0);
  const auto e50 = recs::bootstrap_ci(x, 1000, 42, 0.50);
  const auto e95 = recs::bootstrap_ci(x, 1000, 42, 0.95);
  CHECK(e95.lo <= e50.lo);
  CHECK(e95.hi >= e50.hi);
  CHECK(e50.lo < e50.hi);
}

TEST_CASE("bootstrap_ci coverage of the true mean is close to 95%", "[eval][stats]") {
  // 200 independent normal samples (n = 200, true mean 0.3, sd 1). A 95% percentile bootstrap
  // should contain 0.3 in about 190 of 200 cases; accept 176..198 (88%..99%).
  const double mu = 0.3;
  int contained = 0;
  for (std::uint64_t i = 0; i < 200; ++i) {
    const auto x = normal_sample(recs::mix_seed(1234, i), 200, mu, 1.0);
    const auto e = recs::bootstrap_ci(x, 1000, recs::mix_seed(99, i));
    if (e.lo <= mu && mu <= e.hi) ++contained;
  }
  INFO("contained = " << contained << " / 200");
  CHECK(contained >= 176);
  CHECK(contained <= 198);
}

TEST_CASE("paired_bootstrap", "[eval][stats]") {
  const auto a = normal_sample(31, 150, 0.5, 0.2);
  SECTION("identical vectors: delta 0, CI [0, 0], does not exclude zero") {
    const auto p = recs::paired_bootstrap(a, a);
    CHECK(p.mean_delta == 0.0);
    CHECK(p.lo == 0.0);
    CHECK(p.hi == 0.0);
    CHECK_FALSE(p.excludes_zero);
  }
  SECTION("a + 0.5 vs a: every delta is 0.5, so the CI is [0.5, 0.5] and excludes zero") {
    std::vector<double> b = a;
    for (auto& v : b) v += 0.5;
    const auto p = recs::paired_bootstrap(b, a);
    CHECK_THAT(p.mean_delta, WithinAbs(0.5, 1e-12));
    CHECK_THAT(p.lo, WithinAbs(0.5, 1e-12));
    CHECK_THAT(p.hi, WithinAbs(0.5, 1e-12));
    CHECK(p.excludes_zero);
    const auto q = recs::paired_bootstrap(a, b);  // and the other direction: CI below zero
    CHECK_THAT(q.hi, WithinAbs(-0.5, 1e-12));
    CHECK(q.excludes_zero);
  }
  SECTION("equals bootstrap_ci on the per-user deltas") {
    const auto noise = normal_sample(32, 150, 0.0, 0.01);
    std::vector<double> b(a.size()), d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) b[i] = a[i] + noise[i];
    for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
    const auto p = recs::paired_bootstrap(a, b, 1000, 42);
    const auto e = recs::bootstrap_ci(d, 1000, 42);
    CHECK(p.mean_delta == e.mean);
    CHECK(p.lo == e.lo);
    CHECK(p.hi == e.hi);
    CHECK(p.excludes_zero == (e.lo > 0.0 || e.hi < 0.0));
  }
  SECTION("size mismatch throws") {
    const std::vector<double> b(a.size() + 1, 0.0);
    CHECK_THROWS_AS(recs::paired_bootstrap(a, b), std::invalid_argument);
  }
}

TEST_CASE("cis_disjoint", "[eval][stats]") {
  const recs::Estimate a{0.5, 0.0, 1.0};
  CHECK(recs::cis_disjoint(a, {1.7, 1.5, 2.0}));
  CHECK(recs::cis_disjoint({1.7, 1.5, 2.0}, a));
  CHECK_FALSE(recs::cis_disjoint(a, {1.5, 1.0, 2.0}));   // touching endpoints overlap
  CHECK_FALSE(recs::cis_disjoint(a, {0.6, 0.5, 0.7}));   // nested
  CHECK_FALSE(recs::cis_disjoint(a, {0.0, -1.0, 0.2}));  // partial overlap
}

TEST_CASE("bootstrap_ratio_ci with sqrt_result reproduces RMSE", "[eval][stats]") {
  // Three users' held-out errors (pred - actual):
  //   A: +1, -1      -> sum sq = 2, count 2
  //   B: +2          -> sum sq = 4, count 1
  //   C: +1, +1, -1  -> sum sq = 3, count 3
  // Pooled: sum sq = 9, count 6 -> MSE = 1.5 -> RMSE = sqrt(1.5) = 1.2247449.
  const std::vector<double> num{2.0, 4.0, 3.0};
  const std::vector<double> den{2.0, 1.0, 3.0};
  const auto e = recs::bootstrap_ratio_ci(num, den, 1000, 42, 0.95, /*sqrt_result=*/true);
  CHECK_THAT(e.mean, WithinAbs(std::sqrt(1.5), 1e-12));
  CHECK(e.lo <= e.mean);
  CHECK(e.hi >= e.mean);
  // Bounds: the smallest per-user RMSE is sqrt(1) (A, C), the largest sqrt(4) = 2 (B).
  CHECK(e.lo >= 1.0);
  CHECK(e.hi <= 2.0);

  // The same pooled ratings through rating_errors give the same RMSE.
  const std::vector<float> pred{5.0f, 3.0f, 5.0f, 4.0f, 4.0f, 2.0f};
  const std::vector<recs::Rating> act{{0, 4.0f, 0}, {1, 4.0f, 0}, {2, 3.0f, 0},
                                      {3, 3.0f, 0}, {4, 3.0f, 0}, {5, 3.0f, 0}};
  CHECK_THAT(recs::rating_errors(pred, act).rmse, WithinAbs(e.mean, 1e-12));

  // Without sqrt it is the MSE.
  const auto m = recs::bootstrap_ratio_ci(num, den, 1000, 42, 0.95, false);
  CHECK_THAT(m.mean, WithinAbs(1.5, 1e-12));
  CHECK(m.lo <= 1.5);
  CHECK(m.hi >= 1.5);
}

TEST_CASE("bootstrap_ratio_ci uses the same resampling as bootstrap_ci", "[eval][stats]") {
  // With every denominator 1, sum(num) / sum(den) of a resample is its mean, so the two functions
  // must agree exactly for the same seed.
  const auto x = normal_sample(41, 120, 2.0, 1.0);
  const std::vector<double> ones(x.size(), 1.0);
  const auto a = recs::bootstrap_ci(x, 500, 77);
  const auto b = recs::bootstrap_ratio_ci(x, ones, 500, 77);
  CHECK(a.mean == b.mean);
  CHECK(a.lo == b.lo);
  CHECK(a.hi == b.hi);
}

TEST_CASE("bootstrap rejects invalid arguments", "[eval][stats]") {
  const std::vector<double> x{1.0, 2.0};
  CHECK_THROWS_AS(recs::bootstrap_ci(std::vector<double>{}), std::invalid_argument);
  CHECK_THROWS_AS(recs::bootstrap_ci(x, 0), std::invalid_argument);
  CHECK_THROWS_AS(recs::bootstrap_ci(x, 100, 42, 0.0), std::invalid_argument);
  CHECK_THROWS_AS(recs::bootstrap_ci(x, 100, 42, 1.0), std::invalid_argument);
  CHECK_THROWS_AS(recs::bootstrap_ratio_ci(x, std::vector<double>{1.0}), std::invalid_argument);
  CHECK_THROWS_AS(recs::bootstrap_ratio_ci(x, std::vector<double>{0.0, 0.0}), std::invalid_argument);
}
