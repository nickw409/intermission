// An in-memory artifact for recs-model tests and fuzzers.
//
// The catalog holds the movies the contract fixtures (contracts/fixtures/model/) refer to, with the
// fixture's own metadata, plus a few edge cases (accented title, no year / TMDB id, never rated).
// Models: real "pop" and "pop_count" fitted on a small synthetic dataset, and deterministic stand-ins
// for "knn", "content", "mf" and "als" (those models are not implemented on this branch). The
// version string equals the fixtures' X-Model-Version.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bundle.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace recs::testing {

inline constexpr std::string_view kFixtureVersion = "2026-11-09T21-14-03Z";

/// Deterministic stand-in for a personalised model. Item similarity is
/// w(a, b) = 1 / (1 + |a - b|) for a != b; score(i) = sum over rated items r of (rating / 5) * w(r, i).
/// explain() cites the (at most 3) rated items with the largest contribution.
class StandInModel final : public Model {
 public:
  StandInModel(std::string name, std::uint32_t n_items, bool similar) : name_(std::move(name)), n_(n_items), similar_(similar) {}

  std::string_view name() const override { return name_; }
  void fit(const Dataset&, const Config&, const TrainContext&) override {}
  UserState fold_in(std::span<const Rating> ratings) const override {
    UserState u;
    u.ratings.assign(ratings.begin(), ratings.end());
    return u;
  }
  void score(const UserState& u, std::span<float> out) const override {
    for (std::uint32_t i = 0; i < n_; ++i) {
      double s = 0.0;
      for (const Rating& r : u.ratings) s += contribution(r, i);
      out[i] = static_cast<float>(s);
    }
  }
  Explanation explain(const UserState& u, ItemIdx item) const override {
    Explanation e;
    for (const Rating& r : u.ratings) {
      if (r.item != item) e.because.push_back({r.item, static_cast<float>(contribution(r, item))});
    }
    std::sort(e.because.begin(), e.because.end(), [](const Because& a, const Because& b) {
      return a.weight != b.weight ? a.weight > b.weight : a.item < b.item;
    });
    if (e.because.size() > 3) e.because.resize(3);
    return e;
  }
  void save(const fs::path&) const override {}
  void load(const fs::path&) override {}
  std::uint32_t n_items() const override { return n_; }
  Config config() const override { return Config{{"stand_in", true}}; }
  bool supports_similar() const override { return similar_; }
  std::vector<Scored> similar(ItemIdx item, std::size_t k) const override {
    if (!similar_) return Model::similar(item, k);
    std::vector<Scored> out;
    for (ItemIdx j = 0; j < n_; ++j) {
      if (j != item) out.push_back({j, static_cast<float>(w(item, j))});
    }
    std::sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) {
      return a.score != b.score ? a.score > b.score : a.item < b.item;
    });
    if (out.size() > k) out.resize(k);
    return out;
  }

 private:
  static double w(ItemIdx a, ItemIdx b) {
    const double d = a > b ? a - b : b - a;
    return 1.0 / (1.0 + d);
  }
  static double contribution(const Rating& r, ItemIdx i) { return r.item == i ? 0.0 : (r.value / 5.0) * w(r.item, i); }

  std::string name_;
  std::uint32_t n_;
  bool similar_;
};

struct CatalogRow {
  std::int64_t id;
  const char* title;
  std::optional<int> year;
  std::vector<std::string> genres;
  std::optional<std::int64_t> tmdb;
  std::uint32_t count;
  float mean;
};

inline std::vector<CatalogRow> fixture_catalog_rows() {
  // The first twelve rows are copied from contracts/fixtures/model/moviesBatch.json.
  return {
      {260, "Star Wars: Episode IV - A New Hope", 1977, {"Action", "Adventure", "Sci-Fi"}, 11, 251, 4.23f},
      {1196, "Star Wars: Episode V - The Empire Strikes Back", 1980, {"Action", "Adventure", "Sci-Fi"}, 1891, 211, 4.22f},
      {1210, "Star Wars: Episode VI - Return of the Jedi", 1983, {"Action", "Adventure", "Sci-Fi"}, 1892, 196, 4.14f},
      {4993, "Lord of the Rings: The Fellowship of the Ring, The", 2001, {"Adventure", "Fantasy"}, 120, 198, 4.11f},
      {2571, "Matrix, The", 1999, {"Action", "Sci-Fi", "Thriller"}, 603, 278, 4.19f},
      {2959, "Fight Club", 1999, {"Action", "Crime", "Drama", "Thriller"}, 550, 218, 4.27f},
      {296, "Pulp Fiction", 1994, {"Comedy", "Crime", "Drama", "Thriller"}, 680, 307, 4.2f},
      {356, "Forrest Gump", 1994, {"Comedy", "Drama", "Romance", "War"}, 13, 329, 4.16f},
      {593, "Silence of the Lambs, The", 1991, {"Crime", "Horror", "Thriller"}, 274, 279, 4.16f},
      {1, "Toy Story", 1995, {"Adventure", "Animation", "Children", "Comedy", "Fantasy"}, 862, 215, 3.92f},
      {318, "Shawshank Redemption, The", 1994, {"Crime", "Drama"}, 278, 317, 4.43f},
      {480, "Jurassic Park", 1993, {"Action", "Adventure", "Sci-Fi", "Thriller"}, 329, 238, 3.75f},
      // Edge cases.
      {4973, "Amélie (Fabuleux destin d'Amélie Poulain, Le)", 2001, {"Comedy", "Romance"}, 194, 120, 4.18f},
      {1081, "Lone Star", 1996, {"Drama", "Mystery", "Western"}, 26748, 5, 3.9f},
      {7001, "Hamlet, Prince of Denmark", std::nullopt, {"Drama"}, std::nullopt, 2, 3.5f},
      {7002, "Never Rated", 2020, {}, std::nullopt, 0, 0.0f},
      {7003, "Straße der Sehnsucht", 1958, {"Documentary"}, 4242, 1, 3.0f},
  };
}

inline Catalog fixture_catalog() {
  const auto rows = fixture_catalog_rows();
  Catalog c;
  for (const auto& r : rows) {
    c.items.insert(r.id);
    MovieMeta m;
    m.movie_id = r.id;
    m.title = r.title;
    m.year = r.year;
    m.genres = r.genres;
    m.tmdb_id = r.tmdb;
    c.movies.push_back(m);
    c.rating_count.push_back(r.count);
    c.mean_rating.push_back(r.mean);
  }
  const auto n = static_cast<std::uint32_t>(rows.size());
  std::vector<ItemIdx> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](ItemIdx a, ItemIdx b) { return c.rating_count[a] > c.rating_count[b]; });
  c.popularity_rank.assign(n, 0);
  for (std::uint32_t r = 0; r < n; ++r) c.popularity_rank[order[r]] = r + 1;
  return c;
}

/// A LoadedArtifact equivalent to the fixtures' artifact. `with_models` limits the models loaded.
inline LoadedArtifact fixture_artifact(std::vector<std::string> with_models = {"pop", "pop_count", "knn", "content", "mf", "als"},
                                       std::string version = std::string(kFixtureVersion)) {
  LoadedArtifact a;
  a.catalog = fixture_catalog();
  const std::uint32_t n = a.catalog.items.size();

  // A small synthetic training set so that the real popularity models have something to fit:
  // user u rates item i iff (u + i) % 3 != 0, with rating 1 + ((u * 7 + i) % 9) / 2.
  std::vector<Triplet> t;
  const std::uint32_t users = 12;
  for (std::uint32_t u = 0; u < users; ++u) {
    for (std::uint32_t i = 0; i < n; ++i) {
      if ((u + i) % 3 != 0) t.push_back({u, i, 1.0f + static_cast<float>((u * 7 + i) % 9) / 2.0f, 0});
    }
  }
  const Dataset ds = make_dataset("fixture", users, n, std::move(t), a.catalog.movies);

  for (const auto& name : with_models) {
    std::unique_ptr<Model> m;
    if (name == "pop") {
      m = make_popularity();
      m->fit(ds, Config{{"m", 10}}, {});
    } else if (name == "pop_count") {
      m = make_popularity_count();
      m->fit(ds, Config::object(), {});
    } else {
      m = std::make_unique<StandInModel>(name, n, name == "knn" || name == "content");
    }
    a.models.emplace(name, std::move(m));
  }
  a.manifest.version = std::move(version);
  a.manifest.created_at = "2026-11-09T21:14:03Z";
  a.manifest.git_sha = "0288606";
  a.manifest.dataset = "fixture";
  a.manifest.models = with_models;
  a.manifest.default_model = std::find(with_models.begin(), with_models.end(), "als") != with_models.end() ? "als" : with_models.front();
  a.manifest.config = Config::object();
  return a;
}

inline std::shared_ptr<const service::Bundle> fixture_bundle(
    std::vector<std::string> with_models = {"pop", "pop_count", "knn", "content", "mf", "als"},
    std::string version = std::string(kFixtureVersion)) {
  return service::make_bundle(fixture_artifact(std::move(with_models), std::move(version)));
}

}  // namespace recs::testing
