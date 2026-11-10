// Gate 1 (C1): popularity models. Expected values are computed by hand in the comments.
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../report/test_util.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace {

// 3 users x 4 items.
//   u0: i0 = 5, i1 = 3
//   u1: i0 = 4, i2 = 2
//   u2: i0 = 3, i1 = 4
//   i3: no ratings
// Global mean C = (5 + 3 + 4 + 2 + 3 + 4) / 6 = 3.5.
// n = {3, 2, 1, 0}; item sums = {12, 7, 2, 0}.
recs::Dataset fixture() {
  return recs::make_dataset("pop-fixture", 3, 4,
                            {{0, 0, 5.f, 1}, {0, 1, 3.f, 2}, {1, 0, 4.f, 3},
                             {1, 2, 2.f, 4}, {2, 0, 3.f, 5}, {2, 1, 4.f, 6}});
}

std::vector<float> scores_for(const recs::Model& m, std::vector<recs::Rating> ratings) {
  std::vector<float> out(m.n_items());
  m.score(m.fold_in(ratings), out);
  return out;
}

}  // namespace

TEST_CASE("pop: damped mean matches hand-computed values (m = 10, the default)", "[c1][popularity]") {
  auto m = recs::make_popularity();
  CHECK(m->name() == "pop");
  m->fit(fixture(), recs::Config::object(), {});
  CHECK(m->config() == recs::Config{{"m", 10}});
  REQUIRE(m->n_items() == 4);
  const auto s = scores_for(*m, {});
  // (n_i * mean_i + m * C) / (n_i + m) = (sum_i + 35) / (n_i + 10)
  CHECK(s[0] == Catch::Approx(47.0 / 13.0).epsilon(1e-7));
  CHECK(s[1] == Catch::Approx(42.0 / 12.0).epsilon(1e-7));
  CHECK(s[2] == Catch::Approx(37.0 / 11.0).epsilon(1e-7));
  CHECK(s[3] == Catch::Approx(3.5).epsilon(1e-7));  // no ratings -> C
  CHECK_FALSE(m->predicts_ratings());
  CHECK_FALSE(m->supports_trained_scoring());
}

TEST_CASE("pop: m from config", "[c1][popularity]") {
  auto m = recs::make_popularity();
  m->fit(fixture(), recs::Config{{"m", 2}}, {});
  CHECK(m->config() == recs::Config{{"m", 2.0}});
  const auto s = scores_for(*m, {});
  // (sum_i + 2 * 3.5) / (n_i + 2)
  CHECK(s[0] == Catch::Approx(19.0 / 5.0).epsilon(1e-7));
  CHECK(s[1] == Catch::Approx(14.0 / 4.0).epsilon(1e-7));
  CHECK(s[2] == Catch::Approx(9.0 / 3.0).epsilon(1e-7));
  CHECK(s[3] == Catch::Approx(3.5).epsilon(1e-7));
}

TEST_CASE("pop: m = 0 is the raw item mean; unrated items still score C", "[c1][popularity]") {
  auto m = recs::make_popularity();
  m->fit(fixture(), recs::Config{{"m", 0}}, {});
  const auto s = scores_for(*m, {});
  CHECK(s[0] == Catch::Approx(4.0));
  CHECK(s[1] == Catch::Approx(3.5));
  CHECK(s[2] == Catch::Approx(2.0));
  CHECK(s[3] == Catch::Approx(3.5));
}

TEST_CASE("pop: invalid configs are rejected", "[c1][popularity]") {
  auto m = recs::make_popularity();
  CHECK_THROWS_AS(m->fit(fixture(), recs::Config{{"m", -1}}, {}), std::invalid_argument);
  CHECK_THROWS_AS(m->fit(fixture(), recs::Config{{"m", "ten"}}, {}), std::invalid_argument);
  CHECK_THROWS_AS(m->fit(fixture(), recs::Config{{"mm", 10}}, {}), std::invalid_argument);
  CHECK_THROWS_AS(m->fit(fixture(), recs::Config::array(), {}), std::invalid_argument);
  auto c = recs::make_popularity_count();
  CHECK_THROWS_AS(c->fit(fixture(), recs::Config{{"m", 10}}, {}), std::invalid_argument);
}

TEST_CASE("pop_count: scores are rating counts", "[c1][popularity]") {
  auto m = recs::make_popularity_count();
  CHECK(m->name() == "pop_count");
  m->fit(fixture(), recs::Config::object(), {});
  CHECK(m->config() == recs::Config::object());
  CHECK(scores_for(*m, {}) == std::vector<float>{3.f, 2.f, 1.f, 0.f});
}

TEST_CASE("pop: scores do not depend on the user; fold_in keeps the ratings", "[c1][popularity]") {
  for (auto make : {recs::make_popularity, recs::make_popularity_count}) {
    auto m = make();
    m->fit(fixture(), recs::Config::object(), {});
    const std::vector<recs::Rating> r = {{1, 5.f, 10}, {3, 0.5f, 11}};
    const auto st = m->fold_in(r);
    CHECK(st.ratings == r);
    CHECK(scores_for(*m, {}) == scores_for(*m, r));
    CHECK(scores_for(*m, {{0, 1.f, 1}}) == scores_for(*m, r));
    CHECK(m->explain(st, 0).because.empty());
    CHECK(m->explain(st, 1).because.empty());
  }
}

TEST_CASE("pop: save/load round-trip gives identical scores and config", "[c1][popularity]") {
  for (auto make : {recs::make_popularity, recs::make_popularity_count}) {
    recs::testing::TempDir dir("pop-save");
    auto a = make();
    const recs::Config cfg = a->name() == "pop" ? recs::Config{{"m", 3.25}} : recs::Config::object();
    a->fit(fixture(), cfg, {});
    a->save(dir.path());
    auto b = make();
    b->load(dir.path());
    CHECK(b->n_items() == a->n_items());
    CHECK(b->config() == a->config());
    const auto sa = scores_for(*a, {});
    const auto sb = scores_for(*b, {});
    REQUIRE(sa.size() == sb.size());
    for (std::size_t i = 0; i < sa.size(); ++i) CHECK(std::bit_cast<std::uint32_t>(sa[i]) == std::bit_cast<std::uint32_t>(sb[i]));
  }
}

TEST_CASE("pop: load rejects a file of the other variant", "[c1][popularity]") {
  recs::testing::TempDir dir("pop-variant");
  auto a = recs::make_popularity();
  a->fit(fixture(), recs::Config::object(), {});
  a->save(dir.path());
  auto b = recs::make_popularity_count();
  CHECK_THROWS(b->load(dir.path()));
}

TEST_CASE("pop: empty training set scores every item 0 and never NaN", "[c1][popularity]") {
  auto m = recs::make_popularity();
  m->fit(recs::make_dataset("empty", 2, 3, {}), recs::Config::object(), {});
  const auto s = scores_for(*m, {});
  for (float v : s) {
    CHECK_FALSE(std::isnan(v));
    CHECK(v == 0.f);
  }
}
