// Core value types shared by every librecs module. FROZEN after Wave 0: changes go through the orchestrator.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace recs {

using UserIdx = std::uint32_t;  // dense 0-based user index (never leaves the server)
using ItemIdx = std::uint32_t;  // dense 0-based movie index (never leaves the server)
using Config = nlohmann::json;  // hyperparameters; copied verbatim into artifacts and reports

/// One rating by one (implicit) user. `value` is in [0.5, 5.0] in 0.5 steps.
struct Rating {
  ItemIdx item = 0;
  float value = 0.0f;
  std::int64_t ts = 0;  // Unix seconds
  bool operator==(const Rating&) const = default;
};

/// Compressed sparse rows. Used both as CSR (rows = users, cols = items) and as CSC stored as
/// CSR of the transpose (rows = items, cols = users).
/// Invariants: indptr.size() == n_rows + 1, indptr[0] == 0, indptr is non-decreasing,
/// indices within a row are strictly increasing and < n_cols, values/ts have nnz entries.
struct Csr {
  std::uint32_t n_rows = 0;
  std::uint32_t n_cols = 0;
  std::vector<std::uint64_t> indptr{0};
  std::vector<std::uint32_t> indices;
  std::vector<float> values;
  std::vector<std::int64_t> ts;

  std::uint64_t nnz() const { return indices.size(); }
  std::uint32_t row_size(std::uint32_t r) const {
    return static_cast<std::uint32_t>(indptr[r + 1] - indptr[r]);
  }
  std::span<const std::uint32_t> row_indices(std::uint32_t r) const {
    return {indices.data() + indptr[r], row_size(r)};
  }
  std::span<const float> row_values(std::uint32_t r) const {
    return {values.data() + indptr[r], row_size(r)};
  }
  std::span<const std::int64_t> row_ts(std::uint32_t r) const {
    return {ts.data() + indptr[r], row_size(r)};
  }
  bool operator==(const Csr&) const = default;
};

/// One matrix entry before compression.
struct Triplet {
  std::uint32_t row = 0;
  std::uint32_t col = 0;
  float value = 0.0f;
  std::int64_t ts = 0;
};

/// Bidirectional mapping between external (MovieLens) IDs and dense indices.
struct IdMap {
  std::vector<std::int64_t> to_external;                  // dense -> external
  std::unordered_map<std::int64_t, std::uint32_t> to_dense;  // external -> dense

  std::uint32_t size() const { return static_cast<std::uint32_t>(to_external.size()); }
  std::optional<std::uint32_t> find(std::int64_t external) const {
    auto it = to_dense.find(external);
    if (it == to_dense.end()) return std::nullopt;
    return it->second;
  }
  /// Returns the existing dense index, or assigns the next one.
  std::uint32_t insert(std::int64_t external) {
    auto [it, inserted] = to_dense.try_emplace(external, size());
    if (inserted) to_external.push_back(external);
    return it->second;
  }
  bool operator==(const IdMap& o) const { return to_external == o.to_external; }
};

/// Catalog metadata for one movie, indexed by ItemIdx.
struct MovieMeta {
  std::int64_t movie_id = 0;               // MovieLens movieId
  std::string title;                       // title without the trailing " (YYYY)"
  std::optional<int> year;                 // parsed from the trailing "(YYYY)" if present
  std::vector<std::string> genres;         // empty for "(no genres listed)"
  std::optional<std::int64_t> imdb_id;     // from links.csv
  std::optional<std::int64_t> tmdb_id;     // from links.csv (may be missing)
  bool operator==(const MovieMeta&) const = default;
};

/// One free-text tag application from tags.csv. The user is kept as the external ID because
/// tagging users need not appear in ratings.csv.
struct TagApplication {
  std::int64_t user_id = 0;
  ItemIdx item = 0;
  std::string tag;
  std::int64_t ts = 0;
  bool operator==(const TagApplication&) const = default;
};

/// Dense item x tag relevance (tag genome), row-major. Absent in ml-32m.
struct GenomeMatrix {
  std::uint32_t n_items = 0;
  std::uint32_t n_tags = 0;
  std::vector<std::string> tag_names;
  std::vector<float> relevance;  // n_items * n_tags
  bool operator==(const GenomeMatrix&) const = default;
};

/// A full dataset in dense index space.
/// Items: every movie in movies.csv gets an index (in file order), even with zero ratings.
/// Users: every user in ratings.csv gets an index, in order of first appearance.
struct Dataset {
  std::string name;
  IdMap users;
  IdMap items;
  Csr by_user;  // rows = users, cols = items
  Csr by_item;  // rows = items, cols = users; always transpose(by_user)
  std::vector<MovieMeta> movies;  // size == items.size()
  std::vector<TagApplication> tags;
  std::optional<GenomeMatrix> genome;

  std::uint32_t n_users() const { return users.size(); }
  std::uint32_t n_items() const { return items.size(); }
  std::uint64_t n_ratings() const { return by_user.nnz(); }
};

}  // namespace recs
