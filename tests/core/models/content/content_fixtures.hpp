// Shared fixtures for the content-model tests: small hand-made catalogs and seeded random ones.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "recs/dataset.hpp"
#include "recs/model.hpp"
#include "recs/rng.hpp"

namespace recs::testing::content {

inline MovieMeta movie(std::int64_t id, std::vector<std::string> genres) {
  MovieMeta m;
  m.movie_id = id;
  m.title = "Movie " + std::to_string(id);
  m.genres = std::move(genres);
  return m;
}

inline TagApplication tag(std::int64_t user, ItemIdx item, std::string text) {
  TagApplication t;
  t.user_id = user;
  t.item = item;
  t.tag = std::move(text);
  t.ts = 1600000000;
  return t;
}

/// A dataset whose rating matrix gives every user one rating of every item (values do not matter
/// to item features); the catalog is `movies` and the tags `tags`.
inline Dataset catalog(std::vector<MovieMeta> movies, std::vector<TagApplication> tags,
                       std::uint32_t n_users = 2) {
  const auto n_items = static_cast<std::uint32_t>(movies.size());
  std::vector<Triplet> t;
  for (std::uint32_t u = 0; u < n_users; ++u)
    for (std::uint32_t i = 0; i < n_items; ++i)
      t.push_back({u, i, static_cast<float>(1 + (u + i) % 5), 1600000000});
  Dataset ds = make_dataset("content-fixture", n_users, n_items, std::move(t), std::move(movies));
  ds.tags = std::move(tags);
  return ds;
}

inline const std::vector<std::string>& genre_names() {
  static const std::vector<std::string> g{"Action", "Adventure", "Comedy", "Drama",
                                          "Horror", "Romance", "Sci-Fi", "Thriller"};
  return g;
}

/// Random catalog + ratings: each item gets 0-3 genres, 0-6 tag applications drawn from a small
/// vocabulary written in mixed case with stray whitespace; each user rates a random subset with
/// 0.5-step ratings. Some items end up with no features at all.
inline Dataset random_dataset(Rng& rng, std::uint32_t n_users, std::uint32_t n_items, double density) {
  static const std::vector<std::string> vocab{"funny", "Funny ", "dark", "twist ending", "Twist Ending",
                                              "classic", "visually stunning", "slow", "rare-1", "rare-2",
                                              "atmospheric", " ATMOSPHERIC"};
  std::vector<MovieMeta> movies;
  std::vector<TagApplication> tags;
  for (std::uint32_t i = 0; i < n_items; ++i) {
    std::vector<std::string> g;
    const auto n_g = rng.uniform(4);
    for (std::uint64_t k = 0; k < n_g; ++k) {
      const auto& name = genre_names()[rng.uniform(genre_names().size())];
      if (std::find(g.begin(), g.end(), name) == g.end()) g.push_back(name);
    }
    movies.push_back(movie(1000 + i, std::move(g)));
    const auto n_t = rng.uniform(7);
    for (std::uint64_t k = 0; k < n_t; ++k)
      tags.push_back(tag(static_cast<std::int64_t>(rng.uniform(50)), i, vocab[rng.uniform(vocab.size())]));
  }
  std::vector<Triplet> t;
  for (std::uint32_t u = 0; u < n_users; ++u)
    for (std::uint32_t i = 0; i < n_items; ++i)
      if (rng.uniform01() < density)
        t.push_back({u, i, 0.5f * static_cast<float>(1 + rng.uniform(10)), 1600000000});
  Dataset ds = make_dataset("content-random", n_users, n_items, std::move(t), std::move(movies));
  ds.tags = std::move(tags);
  return ds;
}

/// The ratings of training user `u`, in CSR (item-ascending) order.
inline std::vector<Rating> user_ratings(const Dataset& ds, UserIdx u) {
  std::vector<Rating> r;
  const auto idx = ds.by_user.row_indices(u);
  const auto val = ds.by_user.row_values(u);
  const auto ts = ds.by_user.row_ts(u);
  for (std::size_t k = 0; k < idx.size(); ++k) r.push_back({idx[k], val[k], ts[k]});
  return r;
}

inline std::vector<float> scores_of(const Model& m, const UserState& s) {
  std::vector<float> out(m.n_items());
  m.score(s, out);
  return out;
}

/// Top-k by score descending then ItemIdx ascending, skipping rated items and -infinity.
/// (Written here so these tests do not depend on the shared recommend().)
inline std::vector<ItemIdx> top_k(std::span<const float> scores, std::span<const Rating> rated, std::size_t k) {
  std::vector<bool> skip(scores.size(), false);
  for (const auto& r : rated) skip[r.item] = true;
  std::vector<ItemIdx> items;
  for (ItemIdx i = 0; i < scores.size(); ++i)
    if (!skip[i] && scores[i] != -std::numeric_limits<float>::infinity()) items.push_back(i);
  std::sort(items.begin(), items.end(), [&](ItemIdx a, ItemIdx b) {
    if (scores[a] != scores[b]) return scores[a] > scores[b];
    return a < b;
  });
  if (items.size() > k) items.resize(k);
  return items;
}

/// Bitwise float comparison (distinguishes -0.0 from 0.0, treats identical NaN payloads as equal).
inline bool bitwise_equal(std::span<const float> a, std::span<const float> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
           return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
         });
}

/// A scratch directory removed on scope exit.
class ScratchDir {
 public:
  ScratchDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("recs-content-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace recs::testing::content
