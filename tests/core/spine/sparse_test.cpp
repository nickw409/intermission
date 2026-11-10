#include <algorithm>
#include <set>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "property.hpp"
#include "recs/dataset.hpp"
#include "recs/sparse.hpp"

namespace {
std::vector<recs::Triplet> random_triplets(recs::Rng& rng, std::uint32_t rows, std::uint32_t cols) {
  std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
  std::vector<recs::Triplet> out;
  if (rows == 0 || cols == 0) return out;
  const auto n = rng.uniform(static_cast<std::uint64_t>(rows) * cols + 1);
  for (std::uint64_t i = 0; i < n; ++i) {
    const auto r = static_cast<std::uint32_t>(rng.uniform(rows));
    const auto c = static_cast<std::uint32_t>(rng.uniform(cols));
    if (!seen.insert({r, c}).second) continue;
    out.push_back({r, c, static_cast<float>(1 + rng.uniform(10)) / 2.0f, static_cast<std::int64_t>(rng.uniform(1000))});
  }
  return out;
}
}  // namespace

TEST_CASE("CSR -> CSC -> CSR round-trip is identical (property)", "[spine][sparse]") {
  recs::testing::for_seeds(300, 2026, [](recs::Rng& rng, std::uint64_t) {
    const auto rows = static_cast<std::uint32_t>(rng.uniform(12));
    const auto cols = static_cast<std::uint32_t>(rng.uniform(12));
    const auto m = recs::csr_from_triplets(rows, cols, random_triplets(rng, rows, cols));
    REQUIRE_NOTHROW(recs::validate(m));
    const auto t = recs::transpose(m);
    REQUIRE_NOTHROW(recs::validate(t));
    CHECK(t.n_rows == cols);
    CHECK(t.nnz() == m.nnz());
    CHECK(recs::transpose(t) == m);
  });
}

TEST_CASE("csr_from_triplets rejects duplicates and out-of-range entries", "[spine][sparse]") {
  CHECK_THROWS_AS(recs::csr_from_triplets(2, 2, {{0, 1, 1.f, 0}, {0, 1, 2.f, 0}}), recs::SparseError);
  CHECK_THROWS_AS(recs::csr_from_triplets(2, 2, {{2, 0, 1.f, 0}}), recs::SparseError);
  CHECK_THROWS_AS(recs::csr_from_triplets(2, 2, {{0, 2, 1.f, 0}}), recs::SparseError);
}

TEST_CASE("make_dataset builds consistent CSR and CSC", "[spine][dataset]") {
  const auto ds = recs::make_dataset("t", 3, 4, {{2, 3, 4.f, 10}, {0, 1, 3.5f, 5}, {0, 0, 1.f, 1}});
  CHECK(ds.n_users() == 3);
  CHECK(ds.n_items() == 4);
  CHECK(ds.n_ratings() == 3);
  CHECK(ds.by_item == recs::transpose(ds.by_user));
  CHECK(ds.movies.size() == 4);
  CHECK(ds.items.find(3) == 3u);
  CHECK_THROWS_AS(recs::make_dataset("t", 1, 1, {{0, 0, 1.f, 0}, {0, 0, 2.f, 0}}), recs::DatasetError);
}
