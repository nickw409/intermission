// Phase 4 numerical gates for implicit ALS:
//  - each per-row (user or item) solve matches a direct dense solve of the same system to 1e-6;
//  - the objective is non-increasing after every half-step, within a relative tolerance of 1e-9.
// The dense reference here is built from the raw ratings over EVERY item (C^u as a full diagonal),
// independently of the Y^T Y trick and of build_interactions().
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>
#include <catch2/catch_test_macros.hpp>

#include "../../../../core/src/models/als_detail.hpp"
#include "als_test_util.hpp"
#include "property.hpp"
#include "recs/synthetic.hpp"

namespace {

using recs::Dataset;
using recs::Rng;
namespace ad = recs::als_detail;
namespace tals = recs::testing::als;

constexpr float kThreshold = 3.5f;

// Dense rating lookup: value or 0 when unrated.
std::vector<float> dense_ratings(const Dataset& ds) {
  std::vector<float> r(static_cast<std::size_t>(ds.n_users()) * ds.n_items(), 0.0f);
  for (std::uint32_t u = 0; u < ds.n_users(); ++u) {
    const auto items = ds.by_user.row_indices(u);
    const auto values = ds.by_user.row_values(u);
    for (std::size_t j = 0; j < items.size(); ++j) r[static_cast<std::size_t>(u) * ds.n_items() + items[j]] = values[j];
  }
  return r;
}

// Solves (sum_j c_j f_j f_j^T + lambda I) x = sum_j c_j p_j f_j over ALL rows j of F, with
// p_j = [rating_j >= threshold] and c_j = 1 + alpha p_j. Full-pivot LU, a different algorithm from
// the Cholesky under test.
Eigen::VectorXd dense_solve(const std::vector<float>& F, std::size_t n, std::size_t k,
                            const std::vector<float>& ratings_row, double alpha, double lambda) {
  Eigen::MatrixXd A = lambda * Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  Eigen::VectorXd b = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(k));
  for (std::size_t j = 0; j < n; ++j) {
    Eigen::VectorXd f(static_cast<Eigen::Index>(k));
    for (std::size_t a = 0; a < k; ++a) f(static_cast<Eigen::Index>(a)) = F[j * k + a];
    const double p = ratings_row[j] >= kThreshold ? 1.0 : 0.0;
    const double c = 1.0 + alpha * p;
    A += c * f * f.transpose();
    b += c * p * f;
  }
  return A.fullPivLu().solve(b);
}

void check_close(const std::vector<double>& fast, const Eigen::VectorXd& dense) {
  REQUIRE(fast.size() == static_cast<std::size_t>(dense.size()));
  const double scale = std::max(1.0, dense.cwiseAbs().maxCoeff());
  double max_err = 0.0;
  for (std::size_t a = 0; a < fast.size(); ++a)
    max_err = std::max(max_err, std::abs(fast[a] - dense(static_cast<Eigen::Index>(a))));
  INFO("max |fast - dense| = " << max_err << ", scale = " << scale);
  CHECK(max_err <= 1e-6 * scale);
}

// Brute-force objective over every (user, item) pair from the raw ratings.
double brute_objective(const Dataset& ds, const std::vector<float>& X, const std::vector<float>& Y, std::size_t k,
                       double alpha, double lambda) {
  const auto R = dense_ratings(ds);
  double loss = 0.0;
  for (std::uint32_t u = 0; u < ds.n_users(); ++u)
    for (std::uint32_t i = 0; i < ds.n_items(); ++i) {
      double s = 0.0;
      for (std::size_t a = 0; a < k; ++a) s += static_cast<double>(X[u * k + a]) * Y[i * k + a];
      const double p = R[static_cast<std::size_t>(u) * ds.n_items() + i] >= kThreshold ? 1.0 : 0.0;
      const double c = 1.0 + alpha * p;
      loss += c * (p - s) * (p - s);
    }
  double reg = 0.0;
  for (float x : X) reg += static_cast<double>(x) * x;
  for (float y : Y) reg += static_cast<double>(y) * y;
  return loss + lambda * reg;
}

}  // namespace

TEST_CASE("ALS gate: every per-user and per-item solve matches a dense solve to 1e-6", "[als][gate]") {
  recs::testing::for_seeds(6, 4401, [](Rng& rng, std::uint64_t) {
    const auto n_users = static_cast<std::uint32_t>(40 + rng.uniform(40));
    const auto n_items = static_cast<std::uint32_t>(30 + rng.uniform(40));
    const std::size_t k = 1 + rng.uniform(16);
    const double alphas[] = {0.0, 1.0, 40.0};
    const double lambdas[] = {0.01, 0.1, 1.0};
    const double alpha = alphas[rng.uniform(3)];
    const double lambda = lambdas[rng.uniform(3)];
    INFO("users " << n_users << " items " << n_items << " k " << k << " alpha " << alpha << " lambda " << lambda);
    const Dataset ds = tals::random_ratings(n_users, n_items, 0.25, rng);
    const auto data = ad::build_interactions(ds, kThreshold);
    const auto R = dense_ratings(ds);

    // Item side: X fixed at random values.
    const auto X = tals::random_factors(n_users, k, 0.5, rng);
    const auto GX = ad::gram(X, n_users, k);
    for (std::uint32_t i = 0; i < n_items; ++i) {
      std::vector<float> col(n_users);
      for (std::uint32_t u = 0; u < n_users; ++u) col[u] = R[static_cast<std::size_t>(u) * n_items + i];
      check_close(ad::solve_row(GX, X, k, data.by_item.row_indices(i), alpha, lambda),
                  dense_solve(X, n_users, k, col, alpha, lambda));
    }

    // User side: Y from a real item half-step, as in training.
    std::vector<float> Y(static_cast<std::size_t>(n_items) * k);
    ad::half_step(data.by_item, X, k, alpha, lambda, Y);
    const auto GY = ad::gram(Y, n_items, k);
    for (std::uint32_t u = 0; u < n_users; ++u) {
      const std::vector<float> row(R.begin() + static_cast<std::ptrdiff_t>(u) * n_items,
                                   R.begin() + static_cast<std::ptrdiff_t>(u + 1) * n_items);
      check_close(ad::solve_row(GY, Y, k, data.by_user.row_indices(u), alpha, lambda),
                  dense_solve(Y, n_items, k, row, alpha, lambda));
    }

    // half_step stores exactly solve_row's result, rounded to float.
    std::vector<float> X2(static_cast<std::size_t>(n_users) * k);
    ad::half_step(data.by_user, Y, k, alpha, lambda, X2);
    for (std::uint32_t u = 0; u < n_users; ++u) {
      const auto x = ad::solve_row(GY, Y, k, data.by_user.row_indices(u), alpha, lambda);
      for (std::size_t a = 0; a < k; ++a) CHECK(X2[u * k + a] == static_cast<float>(x[a]));
    }
  });
}

TEST_CASE("ALS objective (Y^T Y trick) equals the brute-force sum over every pair", "[als]") {
  recs::testing::for_seeds(6, 4402, [](Rng& rng, std::uint64_t) {
    const auto n_users = static_cast<std::uint32_t>(20 + rng.uniform(30));
    const auto n_items = static_cast<std::uint32_t>(20 + rng.uniform(30));
    const std::size_t k = 1 + rng.uniform(10);
    const double alpha = 0.5 + 40.0 * rng.uniform01();
    const double lambda = 0.01 + rng.uniform01();
    const Dataset ds = tals::random_ratings(n_users, n_items, 0.3, rng);
    const auto data = ad::build_interactions(ds, kThreshold);
    const auto X = tals::random_factors(n_users, k, 0.7, rng);
    const auto Y = tals::random_factors(n_items, k, 0.7, rng);
    const double fast = ad::objective(data, X, Y, k, alpha, lambda);
    const double brute = brute_objective(ds, X, Y, k, alpha, lambda);
    INFO("fast " << fast << " brute " << brute);
    CHECK(std::abs(fast - brute) <= 1e-10 * std::abs(brute));
  });
}

namespace {

// Runs `iters` full iterations exactly as fit() does (items, then users) and checks the
// objective after every half-step against the value before it.
void check_monotone(const Dataset& ds, std::size_t k, double alpha, double lambda, int iters, Rng& rng) {
  const auto data = ad::build_interactions(ds, kThreshold);
  auto X = tals::random_factors(ds.n_users(), k, 0.01, rng);
  auto Y = tals::random_factors(ds.n_items(), k, 0.01, rng);
  double prev = ad::objective(data, X, Y, k, alpha, lambda);
  int half_steps = 0;
  for (int it = 0; it < iters; ++it) {
    for (int side = 0; side < 2; ++side) {
      if (side == 0) ad::half_step(data.by_item, X, k, alpha, lambda, Y);
      else ad::half_step(data.by_user, Y, k, alpha, lambda, X);
      const double cur = ad::objective(data, X, Y, k, alpha, lambda);
      INFO("iteration " << it << (side == 0 ? " items" : " users") << ": " << prev << " -> " << cur
                        << " (relative change " << (cur - prev) / prev << ")");
      CHECK(cur <= prev * (1.0 + 1e-9));
      prev = cur;
      ++half_steps;
    }
  }
  CHECK(half_steps == 2 * iters);
}

}  // namespace

TEST_CASE("ALS gate: objective is non-increasing after every half-step (1e-9 relative)", "[als][gate]") {
  SECTION("random explicit ratings, varied hyperparameters") {
    recs::testing::for_seeds(5, 4403, [](Rng& rng, std::uint64_t) {
      const Dataset ds = tals::random_ratings(static_cast<std::uint32_t>(60 + rng.uniform(60)),
                                              static_cast<std::uint32_t>(40 + rng.uniform(60)), 0.15, rng);
      const double alphas[] = {0.0, 5.0, 40.0};
      const double lambdas[] = {0.01, 0.05, 1.0};
      const std::size_t k = 1 + rng.uniform(20);
      const double alpha = alphas[rng.uniform(3)];
      const double lambda = lambdas[rng.uniform(3)];
      INFO("k " << k << " alpha " << alpha << " lambda " << lambda);
      check_monotone(ds, k, alpha, lambda, 12, rng);
    });
  }
  SECTION("planted implicit data, spec defaults alpha = 40, lambda = 0.05") {
    recs::testing::for_seeds(2, 4404, [](Rng& rng, std::uint64_t seed) {
      const auto planted = recs::synthetic::planted_implicit(300, 200, 5, 0.05, 0.2, seed);
      check_monotone(planted.train, 16, 40.0, 0.05, 15, rng);
    });
  }
}
