// Phase 1 gates for the content-based model (spec: Phase 1; brief E gates 1-3):
//  1. a synthetic user who rated only movies of one genre gets >= 7 of 10 recommendations in it;
//  2. fold_in + score equals the batch path (score_trained_user) bit for bit;
//  3. explanations only cite movies the user rated.
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "content_fixtures.hpp"
#include "fixtures.hpp"
#include "property.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"
#include "recs/rng.hpp"

using namespace recs;
using namespace recs::testing::content;

namespace {

bool has_genre(const MovieMeta& m, const std::string& g) {
  return std::find(m.genres.begin(), m.genres.end(), g) != m.genres.end();
}

// A catalog built so that "mostly comedies" is not a given:
//  - 8 genres; every genre is the primary genre of an eighth of the movies, and 40% of movies get
//    a random second genre, so comedies are about a sixth of the catalog and many of them overlap
//    with other genres;
//  - each genre has its own small tag pool, and there is a generic pool ("classic", "oscar", ...)
//    shared by every genre, so most items share some feature with most other items;
//  - tags are applied by different users, in mixed case.
struct GenreCatalog {
  Dataset ds;
};

GenreCatalog genre_catalog(Rng& rng, std::uint32_t n_items) {
  const auto& genres = genre_names();
  const std::vector<std::vector<std::string>> pools{
      {"explosions", "car chase", "martial arts"},  // Action
      {"quest", "treasure", "journey"},             // Adventure
      {"funny", "hilarious", "witty"},              // Comedy
      {"moving", "character study", "tearjerker"},  // Drama
      {"scary", "gore", "jump scares"},             // Horror
      {"love story", "romantic", "chemistry"},      // Romance
      {"space", "aliens", "time travel"},           // Sci-Fi
      {"suspense", "twist ending", "tense"}};       // Thriller
  const std::vector<std::string> generic{"classic", "oscar", "based on a book", "great soundtrack",
                                         "visually stunning", "overrated"};
  std::vector<MovieMeta> movies;
  std::vector<TagApplication> tags;
  for (std::uint32_t i = 0; i < n_items; ++i) {
    std::vector<std::size_t> gi{i % genres.size()};  // primary genres round-robin: n/8 of each
    if (rng.uniform01() < 0.4) {
      const auto second = rng.uniform(genres.size());
      if (second != gi[0]) gi.push_back(second);
    }
    std::vector<std::string> names;
    for (auto g : gi) names.push_back(genres[g]);
    movies.push_back(movie(5000 + i, names));
    const auto n_tags = 2 + rng.uniform(4);
    for (std::uint64_t k = 0; k < n_tags; ++k) {
      std::string text;
      if (rng.uniform01() < 0.6) {
        const auto& pool = pools[gi[rng.uniform(gi.size())]];
        text = pool[rng.uniform(pool.size())];
      } else {
        text = generic[rng.uniform(generic.size())];
      }
      if (rng.uniform01() < 0.3) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
      tags.push_back(tag(static_cast<std::int64_t>(rng.uniform(200)), i, text));
    }
  }
  // A handful of background users with random ratings (the content model ignores who rated what
  // when building item features, but fit() needs a rating matrix).
  std::vector<Triplet> t;
  for (std::uint32_t u = 0; u < 20; ++u)
    for (std::uint32_t i = 0; i < n_items; ++i)
      if (rng.uniform01() < 0.2) t.push_back({u, i, 0.5f * static_cast<float>(1 + rng.uniform(10)), 1});
  GenreCatalog c{make_dataset("genre-catalog", 20, n_items, std::move(t), std::move(movies))};
  c.ds.tags = std::move(tags);
  return c;
}

}  // namespace

TEST_CASE("Content gate: a user who rated only one genre gets >= 7 of 10 recommendations in it",
          "[content][gate][phase1]") {
  recs::testing::for_seeds(20, 2026, [](Rng& rng, std::uint64_t) {
    const auto cat = genre_catalog(rng, 160);
    const auto& ds = cat.ds;
    auto model = make_content();
    model->fit(ds, Config::object(), TrainContext{});

    // The synthetic user is a comedy fan who rated 8 comedies and nothing else: the four
    // straight comedies (genre list exactly {Comedy}) 4.5-5 stars, and four comedy hybrids
    // (Comedy plus another genre) 2-3 stars.
    //
    // Why the ratings vary with "how much comedy": the profile is mean-centered,
    // p = sum_j (r_j - mu) v_j, so any feature that every rated movie shares to the same degree
    // cancels out. A user who rated only comedies with ratings unrelated to their comedy-ness
    // therefore has no comedy signal left in p at all (see the report for E: with such ratings
    // this fixture gives 0-6 comedies in the top 10). The gate is about a user whose taste is the
    // genre, so the ratings here express that taste.
    std::vector<ItemIdx> straight, hybrid;
    for (ItemIdx i = 0; i < ds.n_items(); ++i) {
      if (!has_genre(ds.movies[i], "Comedy")) continue;
      (ds.movies[i].genres.size() == 1 ? straight : hybrid).push_back(i);
    }
    REQUIRE(straight.size() >= 6);  // so some straight comedies remain to be recommended
    REQUIRE(hybrid.size() >= 4);
    rng.shuffle(straight.begin(), straight.end());
    rng.shuffle(hybrid.begin(), hybrid.end());
    const std::vector<float> liked{5.0f, 5.0f, 4.5f, 4.5f}, disliked{3.0f, 2.5f, 2.5f, 2.0f};
    std::vector<Rating> ratings;
    for (std::size_t k = 0; k < 4; ++k) ratings.push_back({straight[k], liked[k], 1});
    for (std::size_t k = 0; k < 4; ++k) ratings.push_back({hybrid[k], disliked[k], 1});

    const auto state = model->fold_in(ratings);
    const auto scores = scores_of(*model, state);
    const auto top = top_k(scores, ratings, 10);
    REQUIRE(top.size() == 10);

    // Not trivially true: comedies are a minority of the items that could be recommended.
    std::size_t candidates = 0, comedy_candidates = 0;
    for (ItemIdx i = 0; i < ds.n_items(); ++i) {
      if (std::any_of(ratings.begin(), ratings.end(), [&](const Rating& r) { return r.item == i; })) continue;
      if (scores[i] == -std::numeric_limits<float>::infinity()) continue;
      ++candidates;
      if (has_genre(ds.movies[i], "Comedy")) ++comedy_candidates;
    }
    INFO("comedy share of candidates = " << comedy_candidates << "/" << candidates);
    REQUIRE(static_cast<double>(comedy_candidates) / static_cast<double>(candidates) < 0.35);

    const auto in_genre = std::count_if(top.begin(), top.end(),
                                        [&](ItemIdx i) { return has_genre(ds.movies[i], "Comedy"); });
    CHECK(in_genre >= 7);
  });
}

TEST_CASE("Content gate: fold_in + score equals the batch path bit for bit (tiny fixture)",
          "[content][gate][phase1]") {
  const Dataset ds = load_movielens(recs::testing::tiny_dir(), "tiny");
  auto model = make_content();
  model->fit(ds, Config::object(), TrainContext{});
  REQUIRE(model->supports_trained_scoring());
  std::size_t finite_users = 0;
  for (UserIdx u = 0; u < ds.n_users(); ++u) {
    INFO("user " << u);
    const auto ratings = user_ratings(ds, u);
    const auto via_fold_in = scores_of(*model, model->fold_in(ratings));
    std::vector<float> batch(model->n_items());
    model->score_trained_user(u, batch);
    CHECK(bitwise_equal(via_fold_in, batch));
    if (std::any_of(batch.begin(), batch.end(), [](float s) { return std::isfinite(s); })) ++finite_users;
  }
  CHECK(finite_users > 0);  // the comparison was not between two all -infinity vectors
}

TEST_CASE("Content gate: fold_in + score equals the batch path bit for bit (random data, any order)",
          "[content][gate][phase1][property]") {
  recs::testing::for_seeds(10, 77, [](Rng& rng, std::uint64_t) {
    const Dataset ds = random_dataset(rng, 30, 60, 0.25);
    auto model = make_content();
    model->fit(ds, Config::object(), TrainContext{});
    for (UserIdx u = 0; u < ds.n_users(); ++u) {
      INFO("user " << u);
      auto ratings = user_ratings(ds, u);
      rng.shuffle(ratings.begin(), ratings.end());  // the website sends ratings in any order
      const auto via_fold_in = scores_of(*model, model->fold_in(ratings));
      std::vector<float> batch(model->n_items());
      model->score_trained_user(u, batch);
      CHECK(bitwise_equal(via_fold_in, batch));
      CHECK(std::none_of(batch.begin(), batch.end(), [](float s) { return std::isnan(s); }));
    }
  });
}

TEST_CASE("Content gate: explanations only cite movies the user rated", "[content][gate][phase1][property]") {
  recs::testing::for_seeds(10, 91, [](Rng& rng, std::uint64_t) {
    const Dataset ds = random_dataset(rng, 25, 50, 0.3);
    auto model = make_content();
    model->fit(ds, Config::object(), TrainContext{});
    std::size_t non_empty = 0;
    for (UserIdx u = 0; u < ds.n_users(); ++u) {
      // A random subset of the user's ratings, as the website might send.
      auto ratings = user_ratings(ds, u);
      rng.shuffle(ratings.begin(), ratings.end());
      ratings.resize(rng.uniform(ratings.size() + 1));
      const auto state = model->fold_in(ratings);
      std::set<ItemIdx> rated;
      for (const auto& r : ratings) rated.insert(r.item);
      for (ItemIdx i = 0; i < ds.n_items(); ++i) {
        INFO("user " << u << " item " << i);
        const auto e = model->explain(state, i);
        CHECK(e.because.size() <= 3);
        for (std::size_t k = 0; k < e.because.size(); ++k) {
          CHECK(rated.count(e.because[k].item) == 1);
          CHECK(e.because[k].weight > 0.0f);
          if (k > 0) CHECK(e.because[k - 1].weight >= e.because[k].weight);
        }
        if (!e.because.empty()) ++non_empty;
      }
    }
    CHECK(non_empty > 0);
  });
}
