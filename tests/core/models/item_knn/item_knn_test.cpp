// Gate tests for the item-KNN model (workstream D; spec "Models -> Item-KNN" and Phase 1 gates).
//
//  1. Similarity is symmetric and within [-1, 1]; shrinkage never increases |sim|  (property)
//  2. Hand fixture: similarities, scores and explanations computed on paper
//  3. fold_in + score equals the batch path bit for bit                           (property)
//  4. Explanations only cite items the user rated                                 (property)
//  5. min_support respected, lists truncated to M, save/load round-trip identical (property)
//  6. Determinism: identical neighbour lists with 1 and 4 OpenMP threads
#include <omp.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../../../../core/src/models/item_knn_detail.hpp"
#include "../../data/temp_dir.hpp"
#include "fixtures.hpp"
#include "property.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"
#include "recs/rng.hpp"

using Catch::Matchers::WithinAbs;
using recs::Config;
using recs::Dataset;
using recs::ItemIdx;
using recs::Rating;
using recs::Scored;
using recs::Triplet;
using recs::UserIdx;
using recs::item_knn_detail::full_similarity;
using recs::item_knn_detail::neighbour_list;

namespace {

constexpr float kNegInf = -std::numeric_limits<float>::infinity();

// A random explicit dataset: each (u, i) observed with probability `density`, ratings in 0.5 steps.
// Some items get forced high or zero coverage so min_support has something to cut.
Dataset random_dataset(recs::Rng& rng, std::uint32_t n_users, std::uint32_t n_items, double density) {
  std::vector<Triplet> t;
  for (std::uint32_t u = 0; u < n_users; ++u) {
    for (std::uint32_t i = 0; i < n_items; ++i) {
      if (rng.uniform01() < density) {
        const float value = 0.5f * static_cast<float>(1 + rng.uniform(10));  // 0.5 .. 5.0
        t.push_back(Triplet{u, i, value, static_cast<std::int64_t>(rng.uniform(1000))});
      }
    }
  }
  return recs::make_dataset("random", n_users, n_items, std::move(t));
}

struct RandomCase {
  Dataset ds;
  Config cfg;
};

RandomCase random_case(recs::Rng& rng) {
  const auto n_users = static_cast<std::uint32_t>(2 + rng.uniform(40));
  const auto n_items = static_cast<std::uint32_t>(2 + rng.uniform(25));
  const double density = 0.1 + 0.8 * rng.uniform01();
  RandomCase c{random_dataset(rng, n_users, n_items, density), {}};
  const double shrink_choices[] = {0.0, 0.5, 3.0, 100.0};
  c.cfg = Config{{"neighbors", 1 + rng.uniform(n_items)},
                 {"shrinkage", shrink_choices[rng.uniform(4)]},
                 {"min_support", 1 + rng.uniform(5)},
                 {"normalize", rng.uniform(2) == 1},
                 {"positive_only", rng.uniform(2) == 1}};
  return c;
}

std::vector<Rating> training_ratings(const Dataset& ds, UserIdx u) {
  std::vector<Rating> r;
  const auto idx = ds.by_user.row_indices(u);
  const auto val = ds.by_user.row_values(u);
  const auto ts = ds.by_user.row_ts(u);
  for (std::size_t k = 0; k < idx.size(); ++k) r.push_back(Rating{idx[k], val[k], ts[k]});
  return r;
}

// A random fold-in user: a random subset of items with random ratings, in random order.
std::vector<Rating> random_user(recs::Rng& rng, std::uint32_t n_items) {
  std::vector<Rating> r;
  for (ItemIdx i = 0; i < n_items; ++i) {
    if (rng.uniform01() < 0.4) r.push_back(Rating{i, 0.5f * static_cast<float>(1 + rng.uniform(10)), 0});
  }
  rng.shuffle(r.begin(), r.end());
  return r;
}

std::unique_ptr<recs::Model> fit_knn(const Dataset& ds, const Config& cfg) {
  auto m = recs::make_item_knn();
  m->fit(ds, cfg, recs::TrainContext{});
  return m;
}

std::vector<float> score_of(const recs::Model& m, const recs::UserState& u) {
  std::vector<float> out(m.n_items(), 123.0f);
  m.score(u, out);
  return out;
}

bool bitwise_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// ---- hand fixture -------------------------------------------------------------------------------
//
// Items A=0, B=1, C=2, D=3. Config: neighbors 2, shrinkage 1, min_support 2.
//
//   user  ratings              mean  centered (r - mean)
//   u0    A=5 B=3 C=4          4     A=+1 B=-1 C=0
//   u1    A=4 B=2 D=3          3     A=+1 B=-1 D=0
//   u2    A=2 C=4              3     A=-1 C=+1
//   u3    A=2 B=5 C=5          4     A=-2 B=+1 C=+1
//
// Support: A=4, B=3, C=3, D=1 -> D is below min_support 2: no list, never a neighbour.
//
// Adjusted cosine over co-raters, s = sum c_i c_j / (sqrt(sum c_i^2) sqrt(sum c_j^2)):
//   A,B  co-raters u0 u1 u3: num = -1 -1 -2 = -4, |A| = sqrt(1+1+4) = sqrt6, |B| = sqrt(1+1+1) = sqrt3
//        s = -4 / sqrt18 = -0.9428090415820636; n = 3, s' = s * 3/4 = -0.7071067811865477
//   A,C  co-raters u0 u2 u3: num = 0 -1 -2 = -3, |A| = sqrt6, |C| = sqrt(0+1+1) = sqrt2
//        s = -3 / sqrt12 = -0.8660254037844387; n = 3, s' = s * 3/4 = -0.6495190528383290
//   B,C  co-raters u0 u3:    num = 0 + 1 = 1,  |B| = sqrt(1+1) = sqrt2, |C| = sqrt(0+1) = 1
//        s = 1 / sqrt2 = 0.7071067811865475;   n = 2, s' = s * 2/3 = 0.4714045207910316
//
// Neighbour lists (|s'| descending): A: [B, C]  B: [A, C]  C: [A, B]  D: []
//
// Fold-in user: A=2, B=5, D=5 -> mean 4, centered A=-2, B=+1 (D=+1 but D is nobody's neighbour).
//   score(C) = (s'_CA * -2 + s'_CB * 1) / (|s'_CA| + |s'_CB|)
//            = (1.2990381056766580 + 0.4714045207910316) / 1.1209235736293606 = 1.579449900170532
//   score(A) = only B rated:  s'_AB * 1 / |s'_AB| = -1
//   score(B) = only A rated:  s'_BA * -2 / |s'_BA| = 2
//   score(D) = -inf (no neighbour list)
//   explain(C): contributions s' * c / den: A = 1.158899800341064, B = 0.420550099829468 -> [A, B]
//   explain(B): A = 2 -> [A];  explain(A): B = -1 is negative -> []
//
// normalize = false (no division by sum |s'|):
//   score(C) = 1.2990381056766580 + 0.4714045207910316 = 1.7704426264676896
//   score(A) = s'_AB * 1  = -0.7071067811865477
//   score(B) = s'_BA * -2 =  1.4142135623730954
//   score(D) = -inf
//   explain(C): A = 1.299038105676658, B = 0.4714045207910316;  explain(B): A = 1.4142135623730954
//
// positive_only = true (normalize = true): only s'_BC > 0 survives.
//   lists A: []  B: [C]  C: [B]
//   score(A) = -inf (no list), score(B) = -inf (C unrated), score(C) = s'_CB * 1 / |s'_CB| = 1
Dataset hand_dataset() {
  std::vector<Triplet> t = {
      {0, 0, 5.0f, 1}, {0, 1, 3.0f, 2}, {0, 2, 4.0f, 3},  // u0
      {1, 0, 4.0f, 1}, {1, 1, 2.0f, 2}, {1, 3, 3.0f, 3},  // u1
      {2, 0, 2.0f, 1}, {2, 2, 4.0f, 2},                   // u2
      {3, 0, 2.0f, 1}, {3, 1, 5.0f, 2}, {3, 2, 5.0f, 3},  // u3
  };
  return recs::make_dataset("hand", 4, 4, std::move(t));
}

const Config kHandCfg = Config{{"neighbors", 2},     {"shrinkage", 1.0},       {"min_support", 2},
                               {"normalize", true}, {"positive_only", false}};

// The four scoring modes: (normalize, positive_only).
const std::pair<bool, bool> kModes[] = {{true, false}, {true, true}, {false, false}, {false, true}};

Config with_mode(Config c, std::pair<bool, bool> mode) {
  c["normalize"] = mode.first;
  c["positive_only"] = mode.second;
  return c;
}

}  // namespace

// ---- gate 1 ---------------------------------------------------------------------------------------
// Symmetry is checked EXACTLY (==), not within a tolerance: s_ij and s_ji are built from the same
// per-user terms added in the same (ascending user) order, and a*b == b*a in IEEE arithmetic, so
// the two directions are the same floating-point computation.
TEST_CASE("knn gate 1: shrunk similarity is symmetric, within [-1, 1], and never larger than raw",
          "[knn][gate]") {
  recs::testing::for_seeds(150, 0xD001, [](recs::Rng& rng, std::uint64_t) {
    const auto c = random_case(rng);
    const auto rows = full_similarity(c.ds, c.cfg);
    REQUIRE(rows.size() == c.ds.n_items());
    for (ItemIdx i = 0; i < rows.size(); ++i) {
      for (const auto& p : rows[i]) {
        INFO("i = " << i << ", j = " << p.item);
        REQUIRE(p.item != i);
        REQUIRE(std::isfinite(p.raw));
        REQUIRE(std::isfinite(p.shrunk));
        CHECK(p.raw >= -1.0);
        CHECK(p.raw <= 1.0);
        CHECK(p.shrunk >= -1.0);
        CHECK(p.shrunk <= 1.0);
        CHECK(std::abs(p.shrunk) <= std::abs(p.raw));
        CHECK(p.support >= 1);
        // The mirror entry exists and is bit-identical.
        const auto& other = rows[p.item];
        auto it = std::find_if(other.begin(), other.end(),
                               [&](const auto& q) { return q.item == i; });
        REQUIRE(it != other.end());
        CHECK(it->support == p.support);
        CHECK(std::bit_cast<std::uint64_t>(it->raw) == std::bit_cast<std::uint64_t>(p.raw));
        CHECK(std::bit_cast<std::uint64_t>(it->shrunk) == std::bit_cast<std::uint64_t>(p.shrunk));
      }
    }
  });
}

TEST_CASE("knn gate 1: perfectly correlated and anti-correlated items hit +1 and -1", "[knn][gate]") {
  // Two items with identical centered columns (s = +1) and one mirrored (s = -1). With shrinkage 0
  // the shrunk value equals the raw one.
  // u0: A=5 B=5 C=1 D=3 (mean 3.5 -> A=+1.5 B=+1.5 C=-2.5 D=-0.5)
  // u1: A=1 B=1 C=5 D=3 (mean 2.5 -> A=-1.5 B=-1.5 C=+2.5 D=+0.5)
  std::vector<Triplet> t = {{0, 0, 5, 0}, {0, 1, 5, 0}, {0, 2, 1, 0}, {0, 3, 3, 0},
                            {1, 0, 1, 0}, {1, 1, 1, 0}, {1, 2, 5, 0}, {1, 3, 3, 0}};
  const auto ds = recs::make_dataset("pm1", 2, 4, std::move(t));
  const auto rows = full_similarity(ds, Config{{"shrinkage", 0.0}, {"min_support", 1}});
  auto sim = [&](ItemIdx i, ItemIdx j) {
    for (const auto& p : rows[i]) if (p.item == j) return p;
    FAIL("missing pair");
    return recs::item_knn_detail::PairSimilarity{};
  };
  CHECK(sim(0, 1).raw == 1.0);
  CHECK(sim(0, 2).raw == -1.0);
  CHECK(sim(0, 1).shrunk == 1.0);
  CHECK(sim(1, 2).support == 2);
}

// ---- gate 2 ---------------------------------------------------------------------------------------
TEST_CASE("knn gate 2: hand fixture similarities, scores and explanations", "[knn][gate]") {
  const auto ds = hand_dataset();
  const auto rows = full_similarity(ds, kHandCfg);
  REQUIRE(rows.size() == 4);
  REQUIRE(rows[0].size() == 2);
  REQUIRE(rows[1].size() == 2);
  REQUIRE(rows[2].size() == 2);
  CHECK(rows[3].empty());

  // rows are sorted by item: A -> [B, C], B -> [A, C], C -> [A, B]
  CHECK(rows[0][0].item == 1);
  CHECK(rows[0][0].support == 3);
  CHECK_THAT(rows[0][0].raw, WithinAbs(-0.9428090415820636, 1e-12));
  CHECK_THAT(rows[0][0].shrunk, WithinAbs(-0.7071067811865477, 1e-12));
  CHECK(rows[0][1].item == 2);
  CHECK(rows[0][1].support == 3);
  CHECK_THAT(rows[0][1].raw, WithinAbs(-0.8660254037844387, 1e-12));
  CHECK_THAT(rows[0][1].shrunk, WithinAbs(-0.6495190528383290, 1e-12));
  CHECK(rows[1][1].item == 2);
  CHECK(rows[1][1].support == 2);
  CHECK_THAT(rows[1][1].raw, WithinAbs(0.7071067811865475, 1e-12));
  CHECK_THAT(rows[1][1].shrunk, WithinAbs(0.4714045207910316, 1e-12));

  const auto m = fit_knn(ds, kHandCfg);
  CHECK(m->name() == "knn");
  CHECK(m->n_items() == 4);
  CHECK(m->config() == kHandCfg);
  auto items_of = [](const std::vector<Scored>& v) {
    std::vector<ItemIdx> out;
    for (const auto& s : v) out.push_back(s.item);
    return out;
  };
  CHECK(items_of(neighbour_list(*m, 0)) == std::vector<ItemIdx>{1, 2});
  CHECK(items_of(neighbour_list(*m, 1)) == std::vector<ItemIdx>{0, 2});
  CHECK(items_of(neighbour_list(*m, 2)) == std::vector<ItemIdx>{0, 1});
  CHECK(neighbour_list(*m, 3).empty());
  CHECK(neighbour_list(*m, 2)[0].score == static_cast<float>(-0.6495190528383290));

  const std::vector<Rating> user = {{3, 5.0f, 0}, {0, 2.0f, 0}, {1, 5.0f, 0}};  // unsorted on purpose
  const auto st = m->fold_in(user);
  CHECK(st.mean == 4.0);
  const auto s = score_of(*m, st);
  CHECK_THAT(s[0], WithinAbs(-1.0, 1e-6));
  CHECK_THAT(s[1], WithinAbs(2.0, 1e-6));
  CHECK_THAT(s[2], WithinAbs(1.579449900170532, 1e-6));
  CHECK(s[3] == kNegInf);

  const auto ec = m->explain(st, 2);
  REQUIRE(ec.because.size() == 2);
  CHECK(ec.because[0].item == 0);
  CHECK_THAT(ec.because[0].weight, WithinAbs(1.158899800341064, 1e-6));
  CHECK(ec.because[1].item == 1);
  CHECK_THAT(ec.because[1].weight, WithinAbs(0.420550099829468, 1e-6));
  const auto eb = m->explain(st, 1);
  REQUIRE(eb.because.size() == 1);
  CHECK(eb.because[0].item == 0);
  CHECK_THAT(eb.because[0].weight, WithinAbs(2.0, 1e-6));
  CHECK(m->explain(st, 0).because.empty());
  CHECK(m->explain(st, 3).because.empty());

  // similar(): positive neighbours only, best first.
  REQUIRE(m->supports_similar());
  const auto simC = m->similar(2, 10);
  REQUIRE(simC.size() == 1);
  CHECK(simC[0].item == 1);
  CHECK(m->similar(0, 10).empty());
  CHECK_THROWS_AS(m->similar(4, 1), std::out_of_range);

  // neighbors = 1 keeps only the largest |s'|: A -> B, B -> A, C -> A.
  Config one = kHandCfg;
  one["neighbors"] = 1;
  const auto m1 = fit_knn(ds, one);
  CHECK(items_of(neighbour_list(*m1, 0)) == std::vector<ItemIdx>{1});
  CHECK(items_of(neighbour_list(*m1, 1)) == std::vector<ItemIdx>{0});
  CHECK(items_of(neighbour_list(*m1, 2)) == std::vector<ItemIdx>{0});

  // normalize = false: the plain weighted sum.
  Config raw = kHandCfg;
  raw["normalize"] = false;
  const auto mr = fit_knn(ds, raw);
  CHECK(mr->config() == raw);
  const auto str = mr->fold_in(user);
  const auto sr = score_of(*mr, str);
  CHECK_THAT(sr[0], WithinAbs(-0.7071067811865477, 1e-6));
  CHECK_THAT(sr[1], WithinAbs(1.4142135623730954, 1e-6));
  CHECK_THAT(sr[2], WithinAbs(1.7704426264676896, 1e-6));
  CHECK(sr[3] == kNegInf);
  const auto erc = mr->explain(str, 2);
  REQUIRE(erc.because.size() == 2);
  CHECK(erc.because[0].item == 0);
  CHECK_THAT(erc.because[0].weight, WithinAbs(1.299038105676658, 1e-6));
  CHECK(erc.because[1].item == 1);
  CHECK_THAT(erc.because[1].weight, WithinAbs(0.4714045207910316, 1e-6));
  const auto erb = mr->explain(str, 1);
  REQUIRE(erb.because.size() == 1);
  CHECK_THAT(erb.because[0].weight, WithinAbs(1.4142135623730954, 1e-6));
  CHECK(mr->explain(str, 0).because.empty());

  // positive_only = true: negative neighbours are dropped before truncation.
  Config pos = kHandCfg;
  pos["positive_only"] = true;
  const auto mp = fit_knn(ds, pos);
  CHECK(neighbour_list(*mp, 0).empty());
  CHECK(items_of(neighbour_list(*mp, 1)) == std::vector<ItemIdx>{2});
  CHECK(items_of(neighbour_list(*mp, 2)) == std::vector<ItemIdx>{1});
  const auto sp = score_of(*mp, mp->fold_in(user));
  CHECK(sp[0] == kNegInf);
  CHECK(sp[1] == kNegInf);
  CHECK_THAT(sp[2], WithinAbs(1.0, 1e-6));
  CHECK(sp[3] == kNegInf);
}

TEST_CASE("knn: config defaults and validation", "[knn]") {
  const auto ds = hand_dataset();
  const auto m = fit_knn(ds, Config::object());
  CHECK(m->config() == Config{{"neighbors", 100}, {"shrinkage", 100.0}, {"min_support", 20},
                              {"normalize", true}, {"positive_only", false}});
  // Every item is below the default min_support 20: nothing can be scored.
  const auto s = score_of(*m, m->fold_in(training_ratings(ds, 0)));
  for (float v : s) CHECK(v == kNegInf);

  auto bad = [&](Config c) { CHECK_THROWS_AS(recs::make_item_knn()->fit(ds, c, {}), std::invalid_argument); };
  bad(Config{{"neighbors", 0}});
  bad(Config{{"min_support", 0}});
  bad(Config{{"shrinkage", -1.0}});
  bad(Config{{"neighbours", 10}});  // typo: unknown key
  bad(Config{{"neighbors", "ten"}});
  bad(Config{{"normalize", 1}});
  bad(Config{{"normalize", "false"}});
  bad(Config{{"positive_only", 0}});
  bad(Config{{"positive_only", nullptr}});
}

// ---- gate 3 ---------------------------------------------------------------------------------------
TEST_CASE("knn gate 3: fold_in + score equals the batch path bit for bit (tiny fixture)", "[knn][gate]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  std::vector<Config> configs;  // two neighbourhood settings x the four scoring modes
  for (const auto& base : {Config{{"neighbors", 5}, {"shrinkage", 2.0}, {"min_support", 2}},
                           Config{{"neighbors", 100}, {"shrinkage", 100.0}, {"min_support", 1}}}) {
    for (const auto& mode : kModes) configs.push_back(with_mode(base, mode));
  }
  for (const auto& cfg : configs) {
    INFO("config " << cfg.dump());
    const auto m = fit_knn(ds, cfg);
    REQUIRE(m->supports_trained_scoring());
    std::size_t finite = 0;
    for (UserIdx u = 0; u < ds.n_users(); ++u) {
      INFO("user " << u);
      auto ratings = training_ratings(ds, u);
      std::reverse(ratings.begin(), ratings.end());  // order must not matter
      const auto via_fold_in = score_of(*m, m->fold_in(ratings));
      std::vector<float> batch(m->n_items(), 7.0f);
      m->score_trained_user(u, batch);
      CHECK(bitwise_equal(via_fold_in, batch));
      for (float v : batch) {
        CHECK(!std::isnan(v));
        if (std::isfinite(v)) ++finite;
      }
    }
    CHECK(finite > 0);  // the check is not vacuous
  }
}

TEST_CASE("knn gate 3: fold_in + score equals the batch path bit for bit (random datasets)",
          "[knn][gate]") {
  recs::testing::for_seeds(120, 0xD003, [](recs::Rng& rng, std::uint64_t) {
    const auto c = random_case(rng);
    for (const auto& mode : kModes) {
      const auto cfg = with_mode(c.cfg, mode);
      INFO("config " << cfg.dump());
      const auto m = fit_knn(c.ds, cfg);
      for (UserIdx u = 0; u < c.ds.n_users(); ++u) {
        INFO("user " << u);
        auto ratings = training_ratings(c.ds, u);
        rng.shuffle(ratings.begin(), ratings.end());
        const auto via_fold_in = score_of(*m, m->fold_in(ratings));
        std::vector<float> batch(m->n_items(), 7.0f);
        m->score_trained_user(u, batch);
        REQUIRE(bitwise_equal(via_fold_in, batch));
        for (float v : batch) REQUIRE(!std::isnan(v));
      }
    }
  });
}

TEST_CASE("knn: fold_in edge cases", "[knn]") {
  const auto m = fit_knn(hand_dataset(), kHandCfg);
  const auto empty = m->fold_in({});
  CHECK(empty.ratings.empty());
  for (float v : score_of(*m, empty)) CHECK(v == kNegInf);
  CHECK(m->explain(empty, 0).because.empty());
  const std::vector<Rating> dup = {{0, 4.0f, 0}, {0, 3.0f, 0}};
  CHECK_THROWS_AS(m->fold_in(dup), std::invalid_argument);
  const std::vector<Rating> out_of_range = {{4, 4.0f, 0}};
  CHECK_THROWS_AS(m->fold_in(out_of_range), std::invalid_argument);
  std::vector<float> wrong_size(3);
  CHECK_THROWS_AS(m->score(empty, wrong_size), std::invalid_argument);
  std::vector<float> out(4);
  CHECK_THROWS_AS(m->score_trained_user(4, out), std::out_of_range);
}

// ---- gate 4 ---------------------------------------------------------------------------------------
TEST_CASE("knn gate 4: explanations only cite items the user rated", "[knn][gate]") {
  recs::testing::for_seeds(120, 0xD004, [](recs::Rng& rng, std::uint64_t) {
    const auto c = random_case(rng);
    const auto m = fit_knn(c.ds, c.cfg);
    INFO("config " << c.cfg.dump());
    for (int trial = 0; trial < 5; ++trial) {
      const auto ratings = random_user(rng, c.ds.n_items());
      std::set<ItemIdx> rated;
      for (const auto& r : ratings) rated.insert(r.item);
      const auto st = m->fold_in(ratings);
      const auto s = score_of(*m, st);
      for (ItemIdx i = 0; i < m->n_items(); ++i) {
        const auto e = m->explain(st, i);
        CHECK(e.because.size() <= 3);
        if (s[i] == kNegInf) CHECK(e.because.empty());
        float prev = std::numeric_limits<float>::infinity();
        for (const auto& b : e.because) {
          INFO("item " << i << " cites " << b.item);
          CHECK(rated.count(b.item) == 1);
          CHECK(b.item != i);
          CHECK(b.weight > 0.0f);
          CHECK(b.weight <= prev);
          prev = b.weight;
        }
      }
    }
  });
}

// ---- gate 5 ---------------------------------------------------------------------------------------
// The stored list must be exactly the top-M of the untruncated row by |s'| (ties by ItemIdx), with
// zero similarities dropped, and only items meeting min_support may hold or appear in a list.
TEST_CASE("knn gate 5: min_support respected and lists truncated to the top M", "[knn][gate]") {
  recs::testing::for_seeds(120, 0xD005, [](recs::Rng& rng, std::uint64_t) {
    const auto c = random_case(rng);
    const auto m = fit_knn(c.ds, c.cfg);
    const auto rows = full_similarity(c.ds, c.cfg);
    const auto M = c.cfg["neighbors"].get<std::size_t>();
    const auto min_support = c.cfg["min_support"].get<std::uint32_t>();
    const bool positive_only = c.cfg["positive_only"].get<bool>();
    INFO("config " << c.cfg.dump());
    auto eligible = [&](ItemIdx i) { return c.ds.by_item.row_size(i) >= min_support; };
    for (ItemIdx i = 0; i < c.ds.n_items(); ++i) {
      INFO("item " << i);
      const auto list = neighbour_list(*m, i);
      CHECK(list.size() <= M);
      if (!eligible(i)) CHECK(rows[i].empty());
      if (!eligible(i)) CHECK(list.empty());
      for (const auto& p : rows[i]) CHECK(eligible(p.item));

      std::vector<recs::item_knn_detail::PairSimilarity> expected;
      for (const auto& p : rows[i]) {
        if (positive_only ? p.shrunk > 0.0 : p.shrunk != 0.0) expected.push_back(p);
      }
      std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
        if (std::abs(a.shrunk) != std::abs(b.shrunk)) return std::abs(a.shrunk) > std::abs(b.shrunk);
        return a.item < b.item;
      });
      if (expected.size() > M) expected.resize(M);
      REQUIRE(list.size() == expected.size());
      for (std::size_t k = 0; k < list.size(); ++k) {
        CHECK(list[k].item == expected[k].item);
        CHECK(list[k].score == static_cast<float>(expected[k].shrunk));
        CHECK(eligible(list[k].item));
      }
    }
  });
}

TEST_CASE("knn gate 5: save/load round-trip gives identical lists and scores", "[knn][gate]") {
  recs::testing::for_seeds(40, 0xD006, [](recs::Rng& rng, std::uint64_t seed) {
    const auto c = random_case(rng);
    const auto m = fit_knn(c.ds, c.cfg);
    recs::testing::TempDir dir("knn-" + std::to_string(seed));
    m->save(dir.path());
    auto loaded = recs::make_item_knn();
    loaded->load(dir.path());
    CHECK(loaded->n_items() == m->n_items());
    CHECK(loaded->config() == m->config());
    for (ItemIdx i = 0; i < m->n_items(); ++i) CHECK(neighbour_list(*loaded, i) == neighbour_list(*m, i));
    for (int trial = 0; trial < 5; ++trial) {
      const auto ratings = random_user(rng, c.ds.n_items());
      const auto a = score_of(*m, m->fold_in(ratings));
      const auto b = score_of(*loaded, loaded->fold_in(ratings));
      CHECK(bitwise_equal(a, b));
    }
  });
}

TEST_CASE("knn: load rejects a corrupt or truncated file", "[knn]") {
  const auto m = fit_knn(hand_dataset(), kHandCfg);
  recs::testing::TempDir dir("knn-corrupt");
  m->save(dir.path());
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir.path())) files.push_back(e.path());
  REQUIRE(files.size() == 1);
  const auto file = files[0];
  std::string bytes;
  {
    std::ifstream in(file, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), {});
  }
  auto write = [&](const std::string& b) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
  };
  write(bytes.substr(0, bytes.size() - 1));
  CHECK_THROWS(recs::make_item_knn()->load(dir.path()));
  write(bytes + "x");
  CHECK_THROWS(recs::make_item_knn()->load(dir.path()));
  std::string bad_magic = bytes;
  bad_magic[0] = 'X';
  write(bad_magic);
  CHECK_THROWS(recs::make_item_knn()->load(dir.path()));
  write(bytes);
  CHECK_NOTHROW(recs::make_item_knn()->load(dir.path()));
  CHECK_THROWS(recs::make_item_knn()->load(dir.path() / "missing"));
}

// ---- gate 6 ---------------------------------------------------------------------------------------
TEST_CASE("knn gate 6: identical neighbour lists with 1 and 4 OpenMP threads", "[knn][gate]") {
  recs::Rng rng(0xD007);
  const auto ds = random_dataset(rng, 400, 300, 0.15);
  const Config cfg{{"neighbors", 20}, {"shrinkage", 10.0}, {"min_support", 15}};
  const int saved = omp_get_max_threads();
  omp_set_num_threads(1);
  const auto m1 = fit_knn(ds, cfg);
  omp_set_num_threads(4);
  const auto m4 = fit_knn(ds, cfg);
  omp_set_num_threads(saved);
  std::size_t total = 0;
  for (ItemIdx i = 0; i < ds.n_items(); ++i) {
    const auto a = neighbour_list(*m1, i);
    REQUIRE(a == neighbour_list(*m4, i));
    total += a.size();
  }
  CHECK(total > 0);
  // Byte-identical artifacts too.
  recs::testing::TempDir d1("knn-t1"), d4("knn-t4");
  m1->save(d1.path());
  m4->save(d4.path());
  for (const auto& e : std::filesystem::directory_iterator(d1.path())) {
    std::ifstream a(e.path(), std::ios::binary), b(d4.path() / e.path().filename(), std::ios::binary);
    const std::string sa((std::istreambuf_iterator<char>(a)), {}), sb((std::istreambuf_iterator<char>(b)), {});
    CHECK(sa == sb);
  }
}
