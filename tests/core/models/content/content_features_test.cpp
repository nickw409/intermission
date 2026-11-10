// Item features of the content model (brief E gates 4-5): a hand-computed TF-IDF fixture and the
// details of term handling (case folding, min_tag_count, featureless items, the genome path).
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "content_fixtures.hpp"
#include "property.hpp"
#include "recs/model.hpp"

using namespace recs;
using namespace recs::testing::content;
using Catch::Matchers::WithinAbs;

namespace {

constexpr float kNegInf = -std::numeric_limits<float>::infinity();

// The hand fixture.
//
// Items and genres: 0 = [Action], 1 = [Action, Comedy], 2 = [Comedy].
// Tag applications:
//   item 0: "Funny" (user 1), "funny " (user 2), "dark" (user 1)
//   item 1: "FUNNY" (user 3)
//   item 2: "Dark" (user 3), "rare" (user 4)
// After case folding and trimming: item 0 has funny x2 and dark x1, item 1 funny x1, item 2 dark x1
// and rare x1. N = 3 tagged items. df(funny) = 2, df(dark) = 2, df(rare) = 1 < min_tag_count = 2,
// so "rare" is dropped. idf = ln(3/2) for both kept terms.
// Sublinear tf = 1 + ln(count): item 0 -> funny 1 + ln 2 = 1.693147, dark 1.
//
// Feature order: terms sorted ("dark", "funny"), then genres sorted ("Action", "Comedy").
// The tag block of each item is L2-normalized (idf cancels here since both terms share it):
//   item 0: (1, 1.693147) / sqrt(1 + 2.866747) = (0.508542, 0.861037)
//   item 1: (0, 1)
//   item 2: (1, 0)
// Genres are one-hot times genre_weight (1.0); then the whole vector is L2-normalized:
//   v0 = (0.508542, 0.861037, 1, 0) / sqrt(2) = (0.359594, 0.608845, 0.707107, 0)
//   v1 = (0, 1, 1, 1) / sqrt(3)               = (0, 0.577350, 0.577350, 0.577350)
//   v2 = (1, 0, 0, 1) / sqrt(2)               = (0.707107, 0, 0, 0.707107)
// Cosines: v0.v1 = 0.577350 * (0.608845 + 0.707107) = 0.759765
//          v0.v2 = 0.359594 * 0.707107             = 0.254271
//          v1.v2 = 0.577350 * 0.707107             = 0.408248
//
// User rates item 0 = 5 and item 2 = 3: mu = 4, p = v0 - v2 = (-0.347513, 0.608845, 0.707107, -0.707107),
// |p| = sqrt(0.120765 + 0.370692 + 0.5 + 0.5) = 1.221253.
// score(1) = p.v1 / |p| = 0.577350 * 0.608845 / 1.221253 = 0.287833
// score(0) = (1 - 0.254271) / 1.221253 = 0.610626;  score(2) = (0.254271 - 1) / 1.221253 = -0.610626
// explain(1): item 0 contributes (+1)(0.759765)/1.221253 = 0.622120; item 2 contributes
// (-1)(0.408248)/1.221253 = -0.334287 < 0 and is not cited.
Dataset hand_fixture() {
  return catalog({movie(10, {"Action"}), movie(11, {"Action", "Comedy"}), movie(12, {"Comedy"})},
                 {tag(1, 0, "Funny"), tag(2, 0, "funny "), tag(1, 0, "dark"), tag(3, 1, "FUNNY"),
                  tag(3, 2, "Dark"), tag(4, 2, "rare")});
}

}  // namespace

TEST_CASE("Content hand fixture: TF-IDF, cosine, profile, scores and explanation match the hand computation",
          "[content][features]") {
  auto model = make_content();
  model->fit(hand_fixture(), Config::object(), TrainContext{});
  REQUIRE(model->n_items() == 3);
  REQUIRE(model->supports_similar());

  const auto s0 = model->similar(0, 5);
  REQUIRE(s0.size() == 2);
  CHECK(s0[0].item == 1);
  CHECK_THAT(s0[0].score, WithinAbs(0.759765, 1e-5));
  CHECK(s0[1].item == 2);
  CHECK_THAT(s0[1].score, WithinAbs(0.254271, 1e-5));

  const auto s2 = model->similar(2, 1);
  REQUIRE(s2.size() == 1);
  CHECK(s2[0].item == 1);
  CHECK_THAT(s2[0].score, WithinAbs(0.408248, 1e-5));

  const std::vector<Rating> ratings{{2, 3.0f, 1}, {0, 5.0f, 2}};
  const auto state = model->fold_in(ratings);
  CHECK(state.mean == 4.0);
  REQUIRE(state.vec.size() == 4);
  CHECK_THAT(state.vec[0], WithinAbs(-0.347513, 1e-5));
  CHECK_THAT(state.vec[1], WithinAbs(0.608845, 1e-5));
  CHECK_THAT(state.vec[2], WithinAbs(0.707107, 1e-5));
  CHECK_THAT(state.vec[3], WithinAbs(-0.707107, 1e-5));

  const auto scores = scores_of(*model, state);
  CHECK_THAT(scores[0], WithinAbs(0.610626, 1e-5));
  CHECK_THAT(scores[1], WithinAbs(0.287833, 1e-5));
  CHECK_THAT(scores[2], WithinAbs(-0.610626, 1e-5));

  const auto e = model->explain(state, 1);
  REQUIRE(e.because.size() == 1);
  CHECK(e.because[0].item == 0);
  CHECK_THAT(e.because[0].weight, WithinAbs(0.622120, 1e-5));
}

TEST_CASE("Content config: defaults are filled in and bad values are rejected", "[content][features]") {
  auto model = make_content();
  model->fit(hand_fixture(), Config::object(), TrainContext{});
  CHECK(model->config() == Config{{"min_tag_count", 2}, {"genre_weight", 1.0}});
  CHECK(model->name() == "content");

  auto m2 = make_content();
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"min_tag_count", 0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"genre_weight", -1.0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"genre_wieght", 1.0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"min_tag_count", "2"}}, TrainContext{}), std::invalid_argument);
}

TEST_CASE("Content TF-IDF: tags are case-folded and trimmed before counting", "[content][features]") {
  // Two items with no genres share one tag written differently. Without folding each spelling
  // would be used on one item only and dropped by min_tag_count = 2, leaving no features.
  // (Item 2 is tagged too, so that N = 3 > df = 2 and the idf of "sci-fi" is not zero.)
  auto model = make_content();
  model->fit(catalog({movie(1, {}), movie(2, {}), movie(3, {})},
                     {tag(1, 0, "Sci-Fi"), tag(2, 1, "  sci-fi\t"), tag(2, 2, "western")}),
             Config::object(), TrainContext{});
  const auto s = model->similar(0, 5);
  REQUIRE(s.size() == 1);
  CHECK(s[0].item == 1);
  CHECK_THAT(s[0].score, WithinAbs(1.0, 1e-6));
}

TEST_CASE("Content TF-IDF: min_tag_count counts distinct items, not applications", "[content][features]") {
  // "noir" is applied 4 times (in two spellings) but only to item 0, so its df is 1.
  const auto ds = catalog({movie(1, {}), movie(2, {}), movie(3, {})},
                          {tag(1, 0, "noir"), tag(2, 0, "noir"), tag(3, 0, "Noir"), tag(4, 0, "noir"),
                           tag(1, 0, "heist"), tag(1, 1, "heist"), tag(1, 2, "space"), tag(2, 2, "space"),
                           tag(3, 1, "space")});
  // Terms: heist df 2 (items 0, 1), noir df 1, space df 2 (items 1, 2). N = 3.
  SECTION("min_tag_count = 2 keeps heist and space, drops noir") {
    auto model = make_content();
    model->fit(ds, Config{{"min_tag_count", 2}}, TrainContext{});
    // Item 0 = (heist) only, so it is identical in direction to item 1's heist part.
    const auto s0 = model->similar(0, 5);
    REQUIRE(s0.size() == 1);
    CHECK(s0[0].item == 1);
    // item 0 = (1, 0) over (heist, space); item 1 = (1, 1)/sqrt 2 -> cosine 0.707107.
    CHECK_THAT(s0[0].score, WithinAbs(0.707107, 1e-5));
  }
  SECTION("min_tag_count = 3 drops every term, so every item is featureless and scores -inf") {
    auto model = make_content();
    model->fit(ds, Config{{"min_tag_count", 3}}, TrainContext{});
    CHECK(model->similar(0, 5).empty());
    const auto state = model->fold_in(std::vector<Rating>{{0, 5.0f, 1}, {1, 1.0f, 1}});
    for (float s : scores_of(*model, state)) CHECK(s == kNegInf);
  }
}

TEST_CASE("Content: featureless items score -inf; a zero profile scores everything -inf", "[content][features]") {
  // Item 2 has no genres and no tags.
  const auto ds = catalog({movie(1, {"Drama"}), movie(2, {"Comedy"}), movie(3, {}), movie(4, {"Drama", "Comedy"})}, {});
  auto model = make_content();
  model->fit(ds, Config::object(), TrainContext{});

  const auto state = model->fold_in(std::vector<Rating>{{0, 5.0f, 1}, {1, 2.0f, 1}});
  const auto scores = scores_of(*model, state);
  CHECK(std::isfinite(scores[0]));
  CHECK(std::isfinite(scores[1]));
  CHECK(scores[2] == kNegInf);
  CHECK(std::isfinite(scores[3]));
  CHECK(model->explain(state, 2).because.empty());
  CHECK(model->similar(2, 3).empty());

  // Every rating equal to the mean: p = 0, nothing can be scored.
  const auto flat = model->fold_in(std::vector<Rating>{{0, 4.0f, 1}, {1, 4.0f, 1}});
  for (float s : scores_of(*model, flat)) CHECK(s == kNegInf);
  CHECK(model->explain(flat, 3).because.empty());

  // No ratings at all.
  const auto none = model->fold_in(std::vector<Rating>{});
  for (float s : scores_of(*model, none)) CHECK(s == kNegInf);
}

TEST_CASE("Content: genre_weight trades genres off against tags", "[content][features]") {
  // Item 0 and item 1 share a genre; item 0 and item 2 share a tag (item 3 makes idf non-zero).
  const auto ds = catalog({movie(1, {"Drama"}), movie(2, {"Drama"}), movie(3, {"Horror"}), movie(4, {"Western"})},
                          {tag(1, 0, "slow burn"), tag(1, 2, "slow burn"), tag(1, 3, "other"), tag(2, 1, "other")});
  auto by_genre = make_content();
  by_genre->fit(ds, Config{{"genre_weight", 3.0}}, TrainContext{});
  CHECK(by_genre->similar(0, 1).at(0).item == 1);
  auto by_tag = make_content();
  by_tag->fit(ds, Config{{"genre_weight", 0.25}}, TrainContext{});
  CHECK(by_tag->similar(0, 1).at(0).item == 2);
}

TEST_CASE("Content: the tag genome is used instead of tags and genres when present", "[content][features]") {
  // Tags and genres say item 0 ~ item 1; the genome says item 0 ~ item 2. Item 3 has an all-zero
  // genome row (a movie without genome scores) and must be unscoreable even though it has genres.
  auto ds = catalog({movie(1, {"Drama"}), movie(2, {"Drama"}), movie(3, {"Horror"}), movie(4, {"Drama"})},
                    {tag(1, 0, "same"), tag(1, 1, "same"), tag(1, 2, "different")});
  GenomeMatrix g;
  g.n_items = 4;
  g.n_tags = 3;
  g.tag_names = {"t0", "t1", "t2"};
  g.relevance = {0.9f, 0.1f, 0.0f,   // item 0
                 0.0f, 0.1f, 0.9f,   // item 1
                 0.8f, 0.2f, 0.0f,   // item 2
                 0.0f, 0.0f, 0.0f};  // item 3
  ds.genome = g;
  auto model = make_content();
  model->fit(ds, Config::object(), TrainContext{});
  const auto s0 = model->similar(0, 3);
  REQUIRE(s0.size() == 2);
  CHECK(s0[0].item == 2);
  // cos((0.9, 0.1, 0), (0.8, 0.2, 0)) = 0.74 / (sqrt(0.82) sqrt(0.68)) = 0.990992
  CHECK_THAT(s0[0].score, WithinAbs(0.990992, 1e-5));
  CHECK(s0[1].item == 1);
  CHECK(model->similar(3, 3).empty());
  const auto state = model->fold_in(std::vector<Rating>{{0, 5.0f, 1}, {1, 1.0f, 1}});
  CHECK(scores_of(*model, state)[3] == kNegInf);

  // A genome whose shape does not match the catalog is rejected.
  ds.genome->n_items = 3;
  ds.genome->relevance.resize(9);
  auto bad = make_content();
  CHECK_THROWS_AS(bad->fit(ds, Config::object(), TrainContext{}), std::invalid_argument);
}

TEST_CASE("Content: similar() returns positive cosines only, best first, never the item itself",
          "[content][features]") {
  recs::testing::for_seeds(5, 12, [](Rng& rng, std::uint64_t) {
    const auto ds = random_dataset(rng, 5, 40, 0.3);
    auto model = make_content();
    model->fit(ds, Config::object(), TrainContext{});
    for (ItemIdx i = 0; i < ds.n_items(); ++i) {
      const auto s = model->similar(i, 7);
      CHECK(s.size() <= 7);
      for (std::size_t k = 0; k < s.size(); ++k) {
        CHECK(s[k].item != i);
        CHECK(s[k].score > 0.0f);
        CHECK(s[k].score <= 1.0f + 1e-6f);
        if (k > 0) CHECK((s[k - 1].score > s[k].score || (s[k - 1].score == s[k].score && s[k - 1].item < s[k].item)));
      }
    }
    CHECK_THROWS_AS(model->similar(ds.n_items(), 3), std::out_of_range);
  });
}

TEST_CASE("Content: fold_in rejects unknown and duplicate items; score checks the output size",
          "[content][features]") {
  auto model = make_content();
  model->fit(hand_fixture(), Config::object(), TrainContext{});
  CHECK_THROWS_AS(model->fold_in(std::vector<Rating>{{3, 4.0f, 1}}), std::invalid_argument);
  CHECK_THROWS_AS(model->fold_in(std::vector<Rating>{{1, 4.0f, 1}, {1, 3.0f, 2}}), std::invalid_argument);
  const auto state = model->fold_in(std::vector<Rating>{{0, 4.0f, 1}, {1, 2.0f, 1}});
  std::vector<float> wrong(2);
  CHECK_THROWS_AS(model->score(state, wrong), std::invalid_argument);
  CHECK_THROWS_AS(model->explain(state, 3), std::out_of_range);
}
