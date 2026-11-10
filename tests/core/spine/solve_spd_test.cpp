#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "property.hpp"
#include "recs/foldin.hpp"

TEST_CASE("solve_spd solves (A + lambda I) x = b", "[spine][foldin]") {
  recs::testing::for_seeds(20, 99, [](recs::Rng& rng, std::uint64_t) {
    const std::size_t k = 1 + rng.uniform(16);
    std::vector<double> G(k * k), A(k * k, 0.0), b(k);
    for (auto& g : G) g = rng.normal();
    for (std::size_t i = 0; i < k; ++i)
      for (std::size_t j = 0; j < k; ++j)
        for (std::size_t t = 0; t < k; ++t) A[i * k + j] += G[t * k + i] * G[t * k + j];
    for (auto& x : b) x = rng.normal();
    const double lambda = 0.1;
    const auto x = recs::solve_spd(A, b, k, lambda);
    for (std::size_t i = 0; i < k; ++i) {
      double r = lambda * x[i];
      for (std::size_t j = 0; j < k; ++j) r += A[i * k + j] * x[j];
      CHECK(std::abs(r - b[i]) < 1e-8);
    }
  });
}

TEST_CASE("solve_spd rejects indefinite systems", "[spine][foldin]") {
  const std::vector<double> A{-1.0, 0.0, 0.0, -1.0};
  const std::vector<double> b{1.0, 1.0};
  CHECK_THROWS(recs::solve_spd(A, b, 2, 0.0));
}
