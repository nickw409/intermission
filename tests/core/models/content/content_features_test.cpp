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
  // The user-mean centering, whose arithmetic is easy to follow by hand (the default is checked
  // against the same fixture in the next test).
  auto model = make_content();
  model->fit(hand_fixture(), Config{{"centering", "user_mean"}}, TrainContext{});
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

TEST_CASE("Content hand fixture: the default baseline-residual profile matches the hand computation",
          "[content][features]") {
  auto model = make_content();
  model->fit(hand_fixture(), Config::object(), TrainContext{});

  // Training ratings (catalog(): user u rates item i with 1 + (u + i) % 5):
  //   user 0: items 0, 1, 2 -> 1, 2, 3;   user 1: items 0, 1, 2 -> 2, 3, 4.
  // mu = 15 / 6 = 2.5; b_i = sum (r - mu) / (1000 + n_i): b_0 = -2/1002, b_1 = 0, b_2 = 2/1002.
  const double mu = 2.5, b0 = -2.0 / 1002.0, b2 = 2.0 / 1002.0;
  // The user rates item 2 = 3 and item 0 = 5: b_u = ((3 - mu - b2) + (5 - mu - b0)) / (100 + 2).
  const double bu = ((3.0 - mu - b2) + (5.0 - mu - b0)) / 102.0;
  const double w0 = 5.0 - (mu + bu) - b0, w2 = 3.0 - (mu + bu) - b2;
  REQUIRE(w0 > 0.0);
  REQUIRE(w2 > 0.0);  // under the user mean (4.0) item 2 would get weight -1

  // Features (dark, funny, Action, Comedy). Tags have N = 3 tagged movies and df = 2 for both
  // kept terms, so idf is the same and cancels in the normalization.
  const double r2 = std::sqrt(2.0), L = std::sqrt(1.0 + (1.0 + std::log(2.0)) * (1.0 + std::log(2.0)));
  const std::vector<double> v0{1.0 / (r2 * L), (1.0 + std::log(2.0)) / (r2 * L), 1.0 / r2, 0.0};
  const std::vector<double> v1{0.0, 1.0 / std::sqrt(3.0), 1.0 / std::sqrt(3.0), 1.0 / std::sqrt(3.0)};
  const std::vector<double> v2{1.0 / r2, 0.0, 0.0, 1.0 / r2};
  std::vector<double> p(4);
  for (int f = 0; f < 4; ++f) p[f] = w0 * v0[f] + w2 * v2[f];

  const std::vector<Rating> ratings{{2, 3.0f, 1}, {0, 5.0f, 2}};
  const auto state = model->fold_in(ratings);
  CHECK_THAT(state.mean, WithinAbs(mu + bu, 1e-12));
  REQUIRE(state.vec.size() == 4);
  for (int f = 0; f < 4; ++f) CHECK_THAT(state.vec[f], WithinAbs(p[f], 1e-5));

  const auto dot = [](const std::vector<double>& a, const std::vector<double>& b) {
    double d = 0.0;
    for (std::size_t k = 0; k < a.size(); ++k) d += a[k] * b[k];
    return d;
  };
  const double pn = std::sqrt(dot(p, p));
  const auto scores = scores_of(*model, state);
  CHECK_THAT(scores[0], WithinAbs(dot(p, v0) / pn, 1e-5));
  CHECK_THAT(scores[1], WithinAbs(dot(p, v1) / pn, 1e-5));
  CHECK_THAT(scores[2], WithinAbs(dot(p, v2) / pn, 1e-5));

  // Both rated movies now count for item 1: c_j = w_j (v_j . v_1) / |p|.
  const double c0 = w0 * dot(v0, v1) / pn;
  const double c2 = w2 * dot(v2, v1) / pn;
  const auto e = model->explain(state, 1);
  REQUIRE(e.because.size() == 2);
  CHECK(e.because[0].item == 0);
  CHECK_THAT(e.because[0].weight, WithinAbs(c0, 1e-5));
  CHECK(e.because[1].item == 2);
  CHECK_THAT(e.because[1].weight, WithinAbs(c2, 1e-5));
  CHECK_THAT(static_cast<double>(e.because[0].weight + e.because[1].weight), WithinAbs(scores[1], 1e-5));
}

TEST_CASE("Content config: defaults are filled in and bad values are rejected", "[content][features]") {
  auto model = make_content();
  model->fit(hand_fixture(), Config::object(), TrainContext{});
  CHECK(model->config() == Config{{"min_tag_count", 2},
                                  {"genre_weight", 1.0},
                                  {"centering", "baseline"},
                                  {"user_damping", 100.0},
                                  {"item_damping", 1000.0}});
  CHECK(model->name() == "content");
  for (const char* c : {"user_mean", "global_mean", "damped_user_mean", "baseline"}) {
    auto m = make_content();
    m->fit(hand_fixture(), Config{{"centering", c}}, TrainContext{});
    CHECK(m->config().at("centering") == c);
  }

  auto m2 = make_content();
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"min_tag_count", 0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"genre_weight", -1.0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"genre_wieght", 1.0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"min_tag_count", "2"}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"centering", "midpoint"}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"centering", 1}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"user_damping", -1.0}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"item_damping", "10"}}, TrainContext{}), std::invalid_argument);
  CHECK_THROWS_AS(m2->fit(hand_fixture(), Config{{"user_damping", 1e300}}, TrainContext{}), std::invalid_argument);
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

  // With the user-mean centering, every rating equal to the mean gives p = 0: nothing can be scored.
  auto user_mean = make_content();
  user_mean->fit(ds, Config{{"centering", "user_mean"}}, TrainContext{});
  const auto flat = user_mean->fold_in(std::vector<Rating>{{0, 4.0f, 1}, {1, 4.0f, 1}});
  for (float s : scores_of(*user_mean, flat)) CHECK(s == kNegInf);
  CHECK(user_mean->explain(flat, 3).because.empty());
  // The default centering compares them with the catalog average (here below 4), so the same
  // ratings say "likes Drama and Comedy".
  const auto liked = model->fold_in(std::vector<Rating>{{0, 4.0f, 1}, {1, 4.0f, 1}});
  CHECK(std::isfinite(scores_of(*model, liked)[3]));
  CHECK(model->explain(liked, 3).because.size() == 2);

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

TEST_CASE("Content hand fixture: terms with different idf", "[content][features]") {
  // No genres. Item 0: a, b; item 1: a; item 2: b; item 3: b. N = 4.
  // idf(a) = ln(4/2) = 0.693147, idf(b) = ln(4/3) = 0.287682 (tf = 1 everywhere).
  // v0 = (0.693147, 0.287682) / 0.750476 = (0.923610, 0.383333); v1 = (1, 0); v2 = v3 = (0, 1).
  // cos(1, 0) = 0.923610; cos(2, 3) = 1; cos(2, 0) = 0.383333; cos(1, 2) = 0 (not returned).
  auto model = make_content();
  model->fit(catalog({movie(1, {}), movie(2, {}), movie(3, {}), movie(4, {})},
                     {tag(1, 0, "a"), tag(1, 0, "b"), tag(1, 1, "a"), tag(1, 2, "b"), tag(1, 3, "b")}),
             Config::object(), TrainContext{});
  const auto s1 = model->similar(1, 5);
  REQUIRE(s1.size() == 1);
  CHECK(s1[0].item == 0);
  CHECK_THAT(s1[0].score, WithinAbs(0.923610, 1e-5));
  const auto s2 = model->similar(2, 5);
  REQUIRE(s2.size() == 2);
  CHECK(s2[0].item == 3);
  CHECK_THAT(s2[0].score, WithinAbs(1.0, 1e-6));
  CHECK(s2[1].item == 0);
  CHECK_THAT(s2[1].score, WithinAbs(0.383333, 1e-5));
}

TEST_CASE("Content: the profile is accumulated in double precision", "[content][features]") {
  // Item 0 has only the genre "Big" (v0 = (Big: 1)). Items 1..n have the tag "t" and genre "Big"
  // with genre_weight g = 1e-5, so their Big weight is g / sqrt(1 + g^2) ~ 1e-5. Item n+1 has the
  // tag "u" only (so idf(t) = ln((n+1)/n) > 0). The user rates item 0 = 5 and items 1..n = 1:
  //   w0 = 5 - mu ~ 4,  wk = 1 - mu ~ -4/(n+1),  p[Big] = w0 + sum_k wk * Big_k.
  // Each wk * Big_k ~ -2e-8 is below half an ulp of ~4 in float (2.4e-7), so a float accumulator
  // would drop every one of them; their sum (~ -4e-5, about 80 float ulps) survives in double.
  constexpr std::uint32_t n = 2000;
  const double g = 1e-5;
  std::vector<MovieMeta> movies{movie(0, {"Big"})};
  std::vector<TagApplication> tags;
  for (std::uint32_t k = 1; k <= n; ++k) {
    movies.push_back(movie(k, {"Big"}));
    tags.push_back(tag(1, k, "t"));
  }
  movies.push_back(movie(n + 1, {}));
  tags.push_back(tag(1, n + 1, "u"));
  auto model = make_content();
  model->fit(catalog(std::move(movies), std::move(tags)),
             Config{{"min_tag_count", 1}, {"genre_weight", g}, {"centering", "user_mean"}}, TrainContext{});

  std::vector<Rating> ratings{{0, 5.0f, 1}};
  for (std::uint32_t k = 1; k <= n; ++k) ratings.push_back({k, 1.0f, 1});
  const auto state = model->fold_in(ratings);
  REQUIRE(state.vec.size() == 3);  // features: t, u, Big

  const double mu = (5.0 + n) / (n + 1.0);
  const auto big_k = static_cast<float>(g / std::sqrt(1.0 + g * g));  // stored as float
  double expected = 5.0 - mu;
  float float_accumulated = static_cast<float>(5.0 - mu);
  for (std::uint32_t k = 1; k <= n; ++k) {
    expected += (1.0 - mu) * static_cast<double>(big_k);
    float_accumulated += static_cast<float>((1.0 - mu) * static_cast<double>(big_k));
  }
  REQUIRE(std::abs(static_cast<double>(float_accumulated) - expected) > 1e-5);  // the test can tell them apart
  CHECK_THAT(state.vec[2], WithinAbs(expected, 1e-6));
}
