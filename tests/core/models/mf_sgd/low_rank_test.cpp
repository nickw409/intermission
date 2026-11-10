// Tests for the explicit low-rank synthetic generator (recs/synthetic.hpp, workstream F).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>

#include <Eigen/Dense>
#include <catch2/catch_test_macros.hpp>

#include "recs/synthetic.hpp"

using recs::synthetic::low_rank_explicit;

TEST_CASE("low_rank_explicit: shapes, disjoint split and held-out fraction", "[mf][synthetic]") {
  const auto s = low_rank_explicit(200, 150, 5, 0.1, 0.2, 0.25, 7);
  CHECK(s.rank == 5);
  CHECK(s.noise_sd == 0.1);
  CHECK(s.train.n_users() == 200);
  CHECK(s.train.n_items() == 150);
  REQUIRE(s.heldout.size() == 200);

  std::size_t total = 0, held = 0;
  for (std::uint32_t u = 0; u < 200; ++u) {
    const auto items = s.train.by_user.row_indices(u);
    const std::set<std::uint32_t> train_items(items.begin(), items.end());
    for (const auto& r : s.heldout[u]) {
      CHECK(r.item < 150);
      CHECK(train_items.count(r.item) == 0);  // never both train and held out
      CHECK(std::isfinite(r.value));
    }
    CHECK(std::is_sorted(s.heldout[u].begin(), s.heldout[u].end(),
                         [](const auto& a, const auto& b) { return a.item < b.item; }));
    const std::size_t n = train_items.size() + s.heldout[u].size();
    if (n >= 2) CHECK(!train_items.empty());
    total += n;
    held += s.heldout[u].size();
  }
  const double density = static_cast<double>(total) / (200.0 * 150.0);
  CHECK(std::abs(density - 0.2) < 0.02);
  const double frac = static_cast<double>(held) / static_cast<double>(total);
  CHECK(std::abs(frac - 0.25) < 0.02);
}

TEST_CASE("low_rank_explicit is deterministic in its seed", "[mf][synthetic]") {
  const auto a = low_rank_explicit(50, 40, 3, 0.1, 0.3, 0.2, 11);
  const auto b = low_rank_explicit(50, 40, 3, 0.1, 0.3, 0.2, 11);
  const auto c = low_rank_explicit(50, 40, 3, 0.1, 0.3, 0.2, 12);
  CHECK(a.train.by_user == b.train.by_user);
  CHECK(a.heldout == b.heldout);
  CHECK_FALSE(a.train.by_user == c.train.by_user);
}

TEST_CASE("low_rank_explicit without noise is exactly rank r", "[mf][synthetic]") {
  // Fully observed, no noise, nothing held out: the matrix is P Q^T with rank(P) = rank(Q) = 4.
  const std::uint32_t nu = 30, ni = 25, rank = 4;
  const auto s = low_rank_explicit(nu, ni, rank, 0.0, 1.0, 0.0, 3);
  REQUIRE(s.train.n_ratings() == std::uint64_t{nu} * ni);
  Eigen::MatrixXd R(nu, ni);
  for (std::uint32_t u = 0; u < nu; ++u) {
    const auto items = s.train.by_user.row_indices(u);
    const auto vals = s.train.by_user.row_values(u);
    for (std::size_t j = 0; j < items.size(); ++j) R(u, items[j]) = vals[j];
  }
  const Eigen::JacobiSVD<Eigen::MatrixXd> svd(R);
  const auto& sv = svd.singularValues();
  CHECK(sv(rank - 1) > 1e-2 * sv(0));
  // Values are stored as float32, so "zero" singular values sit at float rounding level.
  CHECK(sv(rank) < 1e-5 * sv(0));
}

TEST_CASE("low_rank_explicit signal has unit variance plus noise", "[mf][synthetic]") {
  // p, q ~ N(0, 1/sqrt(rank)) entrywise, so Var(p . q) = rank * (1/sqrt(rank))^2 = 1.
  const auto s = low_rank_explicit(400, 400, 5, 0.5, 0.5, 0.0, 5);
  double sum = 0.0, sumsq = 0.0;
  const auto n = static_cast<double>(s.train.n_ratings());
  for (const float v : s.train.by_user.values) {
    sum += v;
    sumsq += static_cast<double>(v) * v;
  }
  const double mean = sum / n;
  const double var = sumsq / n - mean * mean;
  CHECK(std::abs(mean) < 0.1);
  CHECK(var > 0.8 * 1.25);
  CHECK(var < 1.2 * 1.25);
}
