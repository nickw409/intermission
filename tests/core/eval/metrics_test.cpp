// Gate tests for recs/metrics.hpp (B1 gate 2). Every expected value is computed by hand in the
// comment above it. Conventions (from the header):
//   relevant  <=> held-out rating >= 4.0
//   gain      = rating - 3.5 for relevant items, 0 otherwise
//   discount  = 1 / log2(rank + 1), rank starting at 1
//   d(1) = 1, d(2) = 1/log2(3) = 0.6309297536, d(3) = 1/2, d(4) = 1/log2(5) = 0.4306765581
//   recall@k = hits / min(k, n_rel); precision@k = hits / k
//   AP@k     = (1 / min(k, n_rel)) * sum over hit ranks i <= k of precision@i
#include <cmath>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "recs/dataset.hpp"
#include "recs/metrics.hpp"

using Catch::Matchers::WithinAbs;
using recs::ItemIdx;
using recs::Rating;

namespace {

constexpr double kTol = 1e-12;

// Held-out ratings from (item, value) pairs; timestamps are irrelevant to the metrics.
std::vector<Rating> held(std::initializer_list<std::pair<ItemIdx, float>> xs) {
  std::vector<Rating> out;
  for (auto [item, value] : xs) out.push_back(Rating{item, value, 0});
  return out;
}

const double kLog2_3 = std::log2(3.0);
const double kLog2_5 = std::log2(5.0);

}  // namespace

TEST_CASE("count_relevant uses the >= 4.0 threshold", "[eval][metrics]") {
  // 4.0 and 5.0 are relevant; 3.5 and 0.5 are not -> 2.
  const auto h = held({{1, 4.0f}, {2, 3.5f}, {3, 5.0f}, {4, 0.5f}});
  CHECK(recs::count_relevant(h) == 2);
  CHECK(recs::count_relevant(std::vector<Rating>{}) == 0);
}

TEST_CASE("ndcg_at_k hand fixtures", "[eval][metrics][ndcg]") {
  SECTION("no relevant items -> 0") {
    // 3.5 and 2.0 are both below 4.0: n_rel = 0, so the metric is defined as 0.
    const auto h = held({{1, 3.5f}, {2, 2.0f}});
    const std::vector<ItemIdx> r{1, 2, 3};
    CHECK(recs::ndcg_at_k(r, h, 3) == 0.0);
  }
  SECTION("all relevant, ideal order -> 1") {
    // gains: item1 = 1.5, item2 = 0.5. DCG = 1.5*1 + 0.5/log2(3) = IDCG -> 1.
    const auto h = held({{1, 5.0f}, {2, 4.0f}});
    const std::vector<ItemIdx> r{1, 2};
    CHECK_THAT(recs::ndcg_at_k(r, h, 2), WithinAbs(1.0, kTol));
  }
  SECTION("all relevant, reversed order") {
    // DCG  = 0.5*1 + 1.5/log2(3) = 0.5 + 0.9463946304 = 1.4463946304
    // IDCG = 1.5*1 + 0.5/log2(3) = 1.5 + 0.3154648768 = 1.8154648768
    // NDCG = 1.4463946304 / 1.8154648768 = 0.7967076...
    const auto h = held({{1, 5.0f}, {2, 4.0f}});
    const std::vector<ItemIdx> r{2, 1};
    const double expected = (0.5 + 1.5 / kLog2_3) / (1.5 + 0.5 / kLog2_3);
    CHECK_THAT(recs::ndcg_at_k(r, h, 2), WithinAbs(expected, kTol));
    CHECK_THAT(expected, WithinAbs(0.7967076, 1e-6));
  }
  SECTION("ties in held-out ratings (equal gains)") {
    // Three items rated 4.5, each gain 1.0. Ranked {3, 9, 1}: hits at ranks 1 and 3.
    // DCG  = 1/1 + 0 + 1/2 = 1.5
    // IDCG = 1 + 1/log2(3) + 1/2 = 2.1309297536
    // NDCG = 1.5 / 2.1309297536 = 0.7039180...
    // Swapping which tied item sits at rank 1 or 3 does not change the value.
    const auto h = held({{1, 4.5f}, {2, 4.5f}, {3, 4.5f}});
    const double expected = 1.5 / (1.0 + 1.0 / kLog2_3 + 0.5);
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{3, 9, 1}, h, 3), WithinAbs(expected, kTol));
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{1, 9, 3}, h, 3), WithinAbs(expected, kTol));
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{2, 9, 1}, h, 3), WithinAbs(expected, kTol));
    CHECK_THAT(expected, WithinAbs(0.7039180, 1e-6));
  }
  SECTION("k greater than the ranked list length") {
    // Ranked {7, 1} with k = 10: only item1 (gain 1.5) is hit, at rank 2. Missing ranks are misses.
    // DCG  = 1.5/log2(3) = 0.9463946304
    // IDCG = 1.5 + 0.5/log2(3) = 1.8154648768 (two relevant items; k = 10 truncates nothing)
    // NDCG = 0.5212960...
    const auto h = held({{1, 5.0f}, {2, 4.0f}});
    const std::vector<ItemIdx> r{7, 1};
    const double expected = (1.5 / kLog2_3) / (1.5 + 0.5 / kLog2_3);
    CHECK_THAT(recs::ndcg_at_k(r, h, 10), WithinAbs(expected, kTol));
    CHECK_THAT(expected, WithinAbs(0.5212960, 1e-6));
  }
  SECTION("relevant item ranked below k") {
    // item1 (gain 1.5) sits at rank 3. k = 2: no hit in the first 2 -> 0.
    // k = 3: DCG = 1.5 * 1/2 = 0.75, IDCG = 1.5 -> 0.5.
    const auto h = held({{1, 5.0f}});
    const std::vector<ItemIdx> r{4, 5, 1};
    CHECK(recs::ndcg_at_k(r, h, 2) == 0.0);
    CHECK_THAT(recs::ndcg_at_k(r, h, 3), WithinAbs(0.5, kTol));
  }
  SECTION("ratings exactly at 4.0 and at 3.5") {
    // item1 = 4.0 is relevant (gain 0.5); item2 = 3.5 is not (gain 0).
    // Ranked {2, 1}: DCG = 0 + 0.5/log2(3); IDCG = 0.5 -> NDCG = 1/log2(3) = 0.6309297536.
    const auto h = held({{1, 4.0f}, {2, 3.5f}});
    const std::vector<ItemIdx> r{2, 1};
    CHECK_THAT(recs::ndcg_at_k(r, h, 2), WithinAbs(1.0 / kLog2_3, kTol));
  }
  SECTION("IDCG is truncated to k") {
    // Three items rated 5.0 (gain 1.5 each). k = 1, ranked {1}: DCG = 1.5, IDCG = 1.5 (top 1 only) -> 1.
    const auto h = held({{1, 5.0f}, {2, 5.0f}, {3, 5.0f}});
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{1}, h, 1), WithinAbs(1.0, kTol));
  }
  SECTION("graded gains: higher rating first beats lower rating first") {
    // gains: item1 = 5.0-3.5 = 1.5, item2 = 4.5-3.5 = 1.0, item3 = 4.0-3.5 = 0.5.
    // Ranked {3, 2, 1}, k = 3:
    //   DCG  = 0.5 + 1.0/log2(3) + 1.5/2 = 0.5 + 0.6309297536 + 0.75 = 1.8809297536
    //   IDCG = 1.5 + 1.0/log2(3) + 0.5/2 = 1.5 + 0.6309297536 + 0.25 = 2.3809297536
    //   NDCG = 0.7899980...
    const auto h = held({{1, 5.0f}, {2, 4.5f}, {3, 4.0f}});
    const double expected = (0.5 + 1.0 / kLog2_3 + 0.75) / (1.5 + 1.0 / kLog2_3 + 0.25);
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{3, 2, 1}, h, 3), WithinAbs(expected, kTol));
    CHECK_THAT(expected, WithinAbs(0.7899980, 1e-6));
  }
  SECTION("a repeated item in the ranked list is only a hit once") {
    // Ranked {1, 1}: rank 1 hits item1 (gain 1.5); rank 2 repeats it and counts as a miss.
    // DCG = 1.5, IDCG = 1.5 -> 1.0 (not 1.5 + 1.5/log2(3) over 1.5, which would exceed 1).
    const auto h = held({{1, 5.0f}});
    CHECK_THAT(recs::ndcg_at_k(std::vector<ItemIdx>{1, 1}, h, 2), WithinAbs(1.0, kTol));
  }
}

TEST_CASE("recall_at_k hand fixtures", "[eval][metrics][recall]") {
  SECTION("no relevant items -> 0") {
    const auto h = held({{1, 3.5f}, {2, 3.0f}});
    CHECK(recs::recall_at_k(std::vector<ItemIdx>{1, 2}, h, 2) == 0.0);
  }
  SECTION("all relevant, all found -> 1") {
    // hits = 2, min(k = 3, n_rel = 2) = 2 -> 2/2 = 1.
    const auto h = held({{1, 4.0f}, {2, 5.0f}});
    CHECK_THAT(recs::recall_at_k(std::vector<ItemIdx>{1, 2, 3}, h, 3), WithinAbs(1.0, kTol));
  }
  SECTION("ties in held-out ratings") {
    // Two items rated 4.5. Ranked {2, 8}: hits = 1, min(2, 2) = 2 -> 0.5. Either tied item counts the same.
    const auto h = held({{1, 4.5f}, {2, 4.5f}});
    CHECK_THAT(recs::recall_at_k(std::vector<ItemIdx>{2, 8}, h, 2), WithinAbs(0.5, kTol));
    CHECK_THAT(recs::recall_at_k(std::vector<ItemIdx>{1, 8}, h, 2), WithinAbs(0.5, kTol));
  }
  SECTION("k greater than the ranked list length") {
    // Three relevant, ranked {1}, k = 10: hits = 1, min(10, 3) = 3 -> 1/3.
    const auto h = held({{1, 5.0f}, {2, 4.0f}, {3, 4.0f}});
    CHECK_THAT(recs::recall_at_k(std::vector<ItemIdx>{1}, h, 10), WithinAbs(1.0 / 3.0, kTol));
  }
  SECTION("relevant items ranked below k") {
    // Relevant {1, 2, 3}; ranked {9, 8, 1, 2}.
    // k = 2: hits 0 -> 0.   k = 3: hits 1 / min(3, 3) -> 1/3.   k = 4: hits 2 / 3 -> 2/3.
    const auto h = held({{1, 5.0f}, {2, 5.0f}, {3, 5.0f}});
    const std::vector<ItemIdx> r{9, 8, 1, 2};
    CHECK(recs::recall_at_k(r, h, 2) == 0.0);
    CHECK_THAT(recs::recall_at_k(r, h, 3), WithinAbs(1.0 / 3.0, kTol));
    CHECK_THAT(recs::recall_at_k(r, h, 4), WithinAbs(2.0 / 3.0, kTol));
  }
  SECTION("ratings exactly at 4.0 and at 3.5") {
    // item1 = 4.0 relevant, item2 = 3.5 not. n_rel = 1.
    // Ranked {2, 1}, k = 2: hits 1 / min(2, 1) -> 1.   k = 1: rank 1 is item2 (not relevant) -> 0.
    const auto h = held({{1, 4.0f}, {2, 3.5f}});
    const std::vector<ItemIdx> r{2, 1};
    CHECK_THAT(recs::recall_at_k(r, h, 2), WithinAbs(1.0, kTol));
    CHECK(recs::recall_at_k(r, h, 1) == 0.0);
  }
  SECTION("denominator is min(k, n_rel) when k < n_rel") {
    // Five relevant, ranked {1, 2}, k = 2: hits 2 / min(2, 5) = 1.
    const auto h = held({{1, 5.0f}, {2, 5.0f}, {3, 5.0f}, {4, 5.0f}, {5, 5.0f}});
    CHECK_THAT(recs::recall_at_k(std::vector<ItemIdx>{1, 2}, h, 2), WithinAbs(1.0, kTol));
  }
}

TEST_CASE("precision_at_k hand fixtures", "[eval][metrics][precision]") {
  SECTION("no relevant items -> 0") {
    const auto h = held({{1, 3.5f}});
    CHECK(recs::precision_at_k(std::vector<ItemIdx>{1}, h, 1) == 0.0);
  }
  SECTION("all relevant, all found -> 1") {
    // hits 2 / k 2 = 1.
    const auto h = held({{1, 4.0f}, {2, 5.0f}});
    CHECK_THAT(recs::precision_at_k(std::vector<ItemIdx>{1, 2}, h, 2), WithinAbs(1.0, kTol));
  }
  SECTION("ties in held-out ratings") {
    // Two items rated 4.5; ranked {2, 8}: hits 1 / 2 = 0.5.
    const auto h = held({{1, 4.5f}, {2, 4.5f}});
    CHECK_THAT(recs::precision_at_k(std::vector<ItemIdx>{2, 8}, h, 2), WithinAbs(0.5, kTol));
  }
  SECTION("k greater than the ranked list length: missing ranks are misses") {
    // Ranked {1, 2}, both relevant, k = 10: hits 2 / 10 = 0.2.
    const auto h = held({{1, 5.0f}, {2, 4.0f}});
    CHECK_THAT(recs::precision_at_k(std::vector<ItemIdx>{1, 2}, h, 10), WithinAbs(0.2, kTol));
  }
  SECTION("relevant item ranked below k") {
    // item1 at rank 3. k = 2: 0/2 = 0.   k = 3: 1/3.
    const auto h = held({{1, 5.0f}});
    const std::vector<ItemIdx> r{9, 8, 1};
    CHECK(recs::precision_at_k(r, h, 2) == 0.0);
    CHECK_THAT(recs::precision_at_k(r, h, 3), WithinAbs(1.0 / 3.0, kTol));
  }
  SECTION("ratings exactly at 4.0 and at 3.5") {
    // Ranked {2, 1}: item2 = 3.5 (miss), item1 = 4.0 (hit) -> 1/2.
    const auto h = held({{1, 4.0f}, {2, 3.5f}});
    CHECK_THAT(recs::precision_at_k(std::vector<ItemIdx>{2, 1}, h, 2), WithinAbs(0.5, kTol));
  }
  SECTION("a repeated item is only a hit once") {
    // Ranked {1, 1}, item1 relevant: hits 1 / 2 = 0.5.
    const auto h = held({{1, 5.0f}});
    CHECK_THAT(recs::precision_at_k(std::vector<ItemIdx>{1, 1}, h, 2), WithinAbs(0.5, kTol));
  }
}

TEST_CASE("average_precision_at_k hand fixtures", "[eval][metrics][map]") {
  SECTION("no relevant items -> 0") {
    const auto h = held({{1, 3.5f}, {2, 1.0f}});
    CHECK(recs::average_precision_at_k(std::vector<ItemIdx>{1, 2}, h, 2) == 0.0);
  }
  SECTION("all relevant, perfectly ranked -> 1") {
    // Hits at ranks 1, 2: P@1 = 1, P@2 = 1. Sum = 2, / min(2, 2) -> 1.
    const auto h = held({{1, 4.0f}, {2, 5.0f}});
    CHECK_THAT(recs::average_precision_at_k(std::vector<ItemIdx>{1, 2}, h, 2), WithinAbs(1.0, kTol));
  }
  SECTION("ties in held-out ratings") {
    // Three items rated 4.5. Ranked {9, 1, 8, 2}, k = 4: hits at rank 2 (P@2 = 1/2) and
    // rank 4 (P@4 = 2/4). Sum = 1.0, / min(4, 3) = 3 -> 1/3.
    const auto h = held({{1, 4.5f}, {2, 4.5f}, {3, 4.5f}});
    CHECK_THAT(recs::average_precision_at_k(std::vector<ItemIdx>{9, 1, 8, 2}, h, 4),
               WithinAbs(1.0 / 3.0, kTol));
  }
  SECTION("k greater than the ranked list length") {
    // Ranked {7, 1}, k = 10, relevant {1, 2}: hit at rank 2 (P@2 = 1/2). Sum 0.5 / min(10, 2) -> 0.25.
    const auto h = held({{1, 5.0f}, {2, 4.0f}});
    CHECK_THAT(recs::average_precision_at_k(std::vector<ItemIdx>{7, 1}, h, 10), WithinAbs(0.25, kTol));
  }
  SECTION("relevant item ranked below k") {
    // Relevant {1, 2}; ranked {1, 9, 2}.
    // k = 2: hit at rank 1 (P@1 = 1); item2 at rank 3 is beyond k. Sum 1 / min(2, 2) -> 0.5.
    // k = 3: hits at 1 (P = 1) and 3 (P = 2/3). Sum 5/3 / 2 -> 5/6.
    const auto h = held({{1, 5.0f}, {2, 5.0f}});
    const std::vector<ItemIdx> r{1, 9, 2};
    CHECK_THAT(recs::average_precision_at_k(r, h, 2), WithinAbs(0.5, kTol));
    CHECK_THAT(recs::average_precision_at_k(r, h, 3), WithinAbs(5.0 / 6.0, kTol));
  }
  SECTION("ratings exactly at 4.0 and at 3.5") {
    // item1 = 4.0, item3 = 4.0 relevant; item2 = 3.5 not. Ranked {2, 1, 3}, k = 3:
    // hits at rank 2 (P = 1/2) and rank 3 (P = 2/3). Sum = 7/6, / min(3, 2) = 2 -> 7/12.
    const auto h = held({{1, 4.0f}, {2, 3.5f}, {3, 4.0f}});
    CHECK_THAT(recs::average_precision_at_k(std::vector<ItemIdx>{2, 1, 3}, h, 3),
               WithinAbs(7.0 / 12.0, kTol));
  }
  SECTION("denominator is min(k, n_rel) when k < n_rel") {
    // Four relevant; ranked {1, 5, 2}, k = 3: hits at 1 (P = 1), 3 (P = 2/3). Sum 5/3 / min(3, 4) -> 5/9.
    const auto h = held({{1, 5.0f}, {2, 5.0f}, {3, 5.0f}, {4, 5.0f}});
    CHECK_THAT(recs::average_precision_at_k(std::vector<ItemIdx>{1, 5, 2}, h, 3),
               WithinAbs(5.0 / 9.0, kTol));
  }
}

TEST_CASE("ranking metrics reject k = 0 and duplicate held-out items", "[eval][metrics]") {
  const auto h = held({{1, 5.0f}});
  const std::vector<ItemIdx> r{1};
  CHECK_THROWS_AS(recs::ndcg_at_k(r, h, 0), std::invalid_argument);
  CHECK_THROWS_AS(recs::recall_at_k(r, h, 0), std::invalid_argument);
  CHECK_THROWS_AS(recs::precision_at_k(r, h, 0), std::invalid_argument);
  CHECK_THROWS_AS(recs::average_precision_at_k(r, h, 0), std::invalid_argument);
  const auto dup = held({{1, 5.0f}, {1, 4.0f}});
  CHECK_THROWS_AS(recs::ndcg_at_k(r, dup, 1), std::invalid_argument);
}

TEST_CASE("jaccard_at_k hand fixtures", "[eval][metrics][jaccard]") {
  using V = std::vector<ItemIdx>;
  // both empty -> defined as 1.
  CHECK(recs::jaccard_at_k(V{}, V{}, 10) == 1.0);
  // one empty: |{}| / |{1, 2}| = 0.
  CHECK(recs::jaccard_at_k(V{1, 2}, V{}, 10) == 0.0);
  // identical sets in a different order: 3/3 = 1.
  CHECK_THAT(recs::jaccard_at_k(V{1, 2, 3}, V{3, 1, 2}, 3), WithinAbs(1.0, kTol));
  // {1,2,3} vs {3,2,4}: intersection {2,3} = 2, union {1,2,3,4} = 4 -> 0.5.
  CHECK_THAT(recs::jaccard_at_k(V{1, 2, 3}, V{3, 2, 4}, 3), WithinAbs(0.5, kTol));
  // k = 2 truncates: {1,2} vs {1,5}: intersection 1, union {1,2,5} = 3 -> 1/3 (items 3, 4 ignored).
  CHECK_THAT(recs::jaccard_at_k(V{1, 2, 3, 4}, V{1, 5, 3, 4}, 2), WithinAbs(1.0 / 3.0, kTol));
  // k > lengths: {1} vs {1,2}: 1 / 2 = 0.5.
  CHECK_THAT(recs::jaccard_at_k(V{1}, V{1, 2}, 10), WithinAbs(0.5, kTol));
  // disjoint: 0 / 4 = 0.
  CHECK(recs::jaccard_at_k(V{1, 2}, V{3, 4}, 2) == 0.0);
}

TEST_CASE("catalog_coverage hand fixtures", "[eval][metrics][coverage]") {
  using L = std::vector<std::vector<ItemIdx>>;
  // {0,1} u {1,2} = {0,1,2}: 3 / 10.
  CHECK_THAT(recs::catalog_coverage(L{{0, 1}, {1, 2}}, 10, 2), WithinAbs(0.3, kTol));
  // k = 1: first entries {0, 3}: 2 / 10.
  CHECK_THAT(recs::catalog_coverage(L{{0, 1, 2}, {3, 4, 5}}, 10, 1), WithinAbs(0.2, kTol));
  // no lists: 0 / 10.
  CHECK(recs::catalog_coverage(L{}, 10, 10) == 0.0);
  // k > lengths, repeated item: {0}: 1 / 4.
  CHECK_THAT(recs::catalog_coverage(L{{0}, {0}}, 4, 10), WithinAbs(0.25, kTol));
  // whole catalog: {0,1,2,3}: 4 / 4.
  CHECK_THAT(recs::catalog_coverage(L{{0, 1}, {2, 3}}, 4, 2), WithinAbs(1.0, kTol));
  // an item outside the catalog is a caller bug.
  CHECK_THROWS_AS(recs::catalog_coverage(L{{4}}, 4, 1), std::invalid_argument);
}

TEST_CASE("popularity_ranks hand fixtures", "[eval][metrics][poprank]") {
  SECTION("ties go to the lower ItemIdx") {
    // Rating counts per item: [2, 5, 2, 0, 5].
    // Order by (count desc, item asc): 1 (5), 4 (5), 0 (2), 2 (2), 3 (0).
    // Ranks: item0 = 3, item1 = 1, item2 = 4, item3 = 5, item4 = 2.
    std::vector<recs::Triplet> t;
    auto add = [&](std::uint32_t item, std::uint32_t n) {
      for (std::uint32_t u = 0; u < n; ++u) t.push_back({u, item, 4.0f, 0});
    };
    add(0, 2); add(1, 5); add(2, 2); add(4, 5);
    const auto ds = recs::make_dataset("pop", 5, 5, t);
    CHECK(recs::popularity_ranks(ds.by_item) == std::vector<std::uint32_t>{3, 1, 4, 5, 2});
  }
  SECTION("all tied -> rank = index + 1") {
    const auto ds = recs::make_dataset("pop", 1, 3, {{0, 0, 4.0f, 0}, {0, 1, 4.0f, 0}, {0, 2, 4.0f, 0}});
    CHECK(recs::popularity_ranks(ds.by_item) == std::vector<std::uint32_t>{1, 2, 3});
  }
  SECTION("strictly ordered counts, most rated last") {
    // counts [1, 2, 3] -> item2 rank 1, item1 rank 2, item0 rank 3.
    const auto ds = recs::make_dataset(
        "pop", 3, 3,
        {{0, 0, 1.0f, 0}, {0, 1, 1.0f, 0}, {1, 1, 1.0f, 0}, {0, 2, 1.0f, 0}, {1, 2, 1.0f, 0}, {2, 2, 1.0f, 0}});
    CHECK(recs::popularity_ranks(ds.by_item) == std::vector<std::uint32_t>{3, 2, 1});
  }
  SECTION("unrated items only") {
    const auto ds = recs::make_dataset("pop", 0, 2, {});
    CHECK(recs::popularity_ranks(ds.by_item) == std::vector<std::uint32_t>{1, 2});
  }
  SECTION("empty catalog") {
    const auto ds = recs::make_dataset("pop", 0, 0, {});
    CHECK(recs::popularity_ranks(ds.by_item).empty());
  }
}

TEST_CASE("mean_popularity_rank hand fixtures", "[eval][metrics][poprank]") {
  using L = std::vector<std::vector<ItemIdx>>;
  const std::vector<std::uint32_t> ranks{3, 1, 4, 5, 2};  // item -> rank
  // k = 2: entries 1, 4 | 0, 3 -> ranks 1, 2, 3, 5 -> 11 / 4 = 2.75.
  CHECK_THAT(recs::mean_popularity_rank(L{{1, 4}, {0, 3, 2}}, ranks, 2), WithinAbs(2.75, kTol));
  // k = 10 (> lengths): entries 1, 4, 0, 3, 2 -> 1 + 2 + 3 + 5 + 4 = 15 / 5 = 3.
  CHECK_THAT(recs::mean_popularity_rank(L{{1, 4}, {0, 3, 2}}, ranks, 10), WithinAbs(3.0, kTol));
  // single list {3}: rank 5.
  CHECK_THAT(recs::mean_popularity_rank(L{{3}}, ranks, 10), WithinAbs(5.0, kTol));
  // k = 1 over three lists: firsts 0, 2, 1 -> 3 + 4 + 1 = 8 / 3.
  CHECK_THAT(recs::mean_popularity_rank(L{{0, 1}, {2}, {1, 0}}, ranks, 1), WithinAbs(8.0 / 3.0, kTol));
  // no entries -> 0.
  CHECK(recs::mean_popularity_rank(L{}, ranks, 10) == 0.0);
  CHECK(recs::mean_popularity_rank(L{{}, {}}, ranks, 10) == 0.0);
  // out-of-range item is a caller bug.
  CHECK_THROWS_AS(recs::mean_popularity_rank(L{{5}}, ranks, 1), std::invalid_argument);
}

TEST_CASE("rating_errors hand fixtures", "[eval][metrics][rmse]") {
  SECTION("mixed errors") {
    // errors (pred - actual): 3-4 = -1, 4.5-4 = 0.5, 2-2 = 0.
    // squared: 1, 0.25, 0 -> mean 1.25/3 = 0.4166667 -> RMSE = sqrt(0.4166667) = 0.6454972.
    // absolute: 1, 0.5, 0 -> MAE = 1.5/3 = 0.5.
    const std::vector<float> p{3.0f, 4.5f, 2.0f};
    const auto a = held({{0, 4.0f}, {1, 4.0f}, {2, 2.0f}});
    const auto e = recs::rating_errors(p, a);
    CHECK(e.n == 3);
    CHECK_THAT(e.rmse, WithinAbs(std::sqrt(1.25 / 3.0), kTol));
    CHECK_THAT(e.mae, WithinAbs(0.5, kTol));
  }
  SECTION("four errors -1, 0, 1, 2") {
    // squared 1 + 0 + 1 + 4 = 6 -> RMSE = sqrt(6/4) = sqrt(1.5); abs 1 + 0 + 1 + 2 = 4 -> MAE = 1.
    const std::vector<float> p{1.0f, 2.0f, 3.0f, 4.0f};
    const auto a = held({{0, 2.0f}, {1, 2.0f}, {2, 2.0f}, {3, 2.0f}});
    const auto e = recs::rating_errors(p, a);
    CHECK(e.n == 4);
    CHECK_THAT(e.rmse, WithinAbs(std::sqrt(1.5), kTol));
    CHECK_THAT(e.mae, WithinAbs(1.0, kTol));
  }
  SECTION("perfect predictions -> 0") {
    const std::vector<float> p{4.0f, 0.5f};
    const auto a = held({{0, 4.0f}, {1, 0.5f}});
    const auto e = recs::rating_errors(p, a);
    CHECK(e.rmse == 0.0);
    CHECK(e.mae == 0.0);
    CHECK(e.n == 2);
  }
  SECTION("single error of 4") {
    const std::vector<float> p{5.0f};
    const auto a = held({{0, 1.0f}});
    const auto e = recs::rating_errors(p, a);
    CHECK_THAT(e.rmse, WithinAbs(4.0, kTol));
    CHECK_THAT(e.mae, WithinAbs(4.0, kTol));
  }
  SECTION("empty -> zeros") {
    const auto e = recs::rating_errors(std::vector<float>{}, std::vector<Rating>{});
    CHECK(e.n == 0);
    CHECK(e.rmse == 0.0);
    CHECK(e.mae == 0.0);
  }
  SECTION("size mismatch throws") {
    const std::vector<float> p{1.0f};
    CHECK_THROWS_AS(recs::rating_errors(p, std::vector<Rating>{}), std::invalid_argument);
  }
}

TEST_CASE("roc_auc hand fixtures", "[eval][metrics][auc]") {
  using V = std::vector<ItemIdx>;
  using S = std::vector<float>;
  SECTION("perfect separation -> 1") {
    // positives {0, 1} score 0.9, 0.8; negatives {2, 3} score 0.1, 0.2: every pair is won.
    CHECK_THAT(recs::roc_auc(S{0.9f, 0.8f, 0.1f, 0.2f}, V{0, 1}, V{}), WithinAbs(1.0, kTol));
  }
  SECTION("reversed -> 0") {
    // positives {2, 3} score below every negative: every pair is lost.
    CHECK_THAT(recs::roc_auc(S{0.9f, 0.8f, 0.1f, 0.2f}, V{2, 3}, V{}), WithinAbs(0.0, kTol));
  }
  SECTION("all ties -> 0.5") {
    // every pair is tied and counts 1/2.
    CHECK_THAT(recs::roc_auc(S{0.5f, 0.5f, 0.5f, 0.5f}, V{0, 3}, V{}), WithinAbs(0.5, kTol));
  }
  SECTION("excluded items are ignored") {
    // positive 0 (0.9). Without exclusion negatives {1: 0.1, 2: 0.95, 3: 0.2}: wins 2 of 3 -> 2/3.
    // Excluding item 2 leaves negatives {1, 3}: wins 2 of 2 -> 1.
    const S s{0.9f, 0.1f, 0.95f, 0.2f};
    CHECK_THAT(recs::roc_auc(s, V{0}, V{}), WithinAbs(2.0 / 3.0, kTol));
    CHECK_THAT(recs::roc_auc(s, V{0}, V{2}), WithinAbs(1.0, kTol));
  }
  SECTION("partial ties") {
    // positive 0 (0.5) vs negatives 1 (0.5, tie = 1/2) and 2 (0.3, win = 1): 1.5 / 2 = 0.75.
    CHECK_THAT(recs::roc_auc(S{0.5f, 0.5f, 0.3f}, V{0}, V{}), WithinAbs(0.75, kTol));
  }
  SECTION("mixed ranking") {
    // positives 0 (0.3), 1 (0.6); negatives 2 (0.5), 3 (0.1).
    // pairs: (0,2) lose, (0,3) win, (1,2) win, (1,3) win -> 3/4.
    CHECK_THAT(recs::roc_auc(S{0.3f, 0.6f, 0.5f, 0.1f}, V{0, 1}, V{}), WithinAbs(0.75, kTol));
  }
  SECTION("an empty class -> 0.5") {
    CHECK(recs::roc_auc(S{0.3f, 0.6f}, V{}, V{}) == 0.5);           // no positives
    CHECK(recs::roc_auc(S{0.3f, 0.6f}, V{0}, V{1}) == 0.5);         // no negatives left
    CHECK(recs::roc_auc(S{0.3f, 0.6f}, V{0, 1}, V{}) == 0.5);       // every item positive
  }
  SECTION("a positive that is also excluded is ignored") {
    // scores [0.9, 0.1, 0.95, 0.2]; positives {0, 2}, exclude {2}: item 2 is dropped entirely, so
    // positive 0 (0.9) vs negatives 1 (0.1), 3 (0.2): wins 2 of 2 -> 1.
    // (If item 2 were kept as a positive, the pairs (2,1), (2,3) are also wins -> still 1, so
    // also check a case where keeping it would change the answer:)
    // scores [0.9, 0.1, 0.05, 0.2]; positives {0, 2}, exclude {2}: again 2/2 wins -> 1.
    // Keeping item 2 as a positive would give (0,1) win, (0,3) win, (2,1) loss, (2,3) loss -> 0.5.
    CHECK_THAT(recs::roc_auc(S{0.9f, 0.1f, 0.95f, 0.2f}, V{0, 2}, V{2}), WithinAbs(1.0, kTol));
    CHECK_THAT(recs::roc_auc(S{0.9f, 0.1f, 0.05f, 0.2f}, V{0, 2}, V{2}), WithinAbs(1.0, kTol));
    // Every positive excluded -> no positives left -> 0.5.
    CHECK(recs::roc_auc(S{0.3f, 0.1f}, V{0}, V{0}) == 0.5);
  }
  SECTION("invalid input throws") {
    CHECK_THROWS_AS(recs::roc_auc(S{0.3f}, V{1}, V{}), std::invalid_argument);   // out of range
    CHECK_THROWS_AS(recs::roc_auc(S{0.3f}, V{}, V{1}), std::invalid_argument);   // out of range
  }
}
