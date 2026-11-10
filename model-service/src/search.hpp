// Title search: case- and accent-insensitive prefix and substring matching.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "recs/artifact.hpp"

namespace recs::service {

/// Normalizes text for matching: Latin letters are lower-cased and stripped of accents
/// ("Amélie" -> "amelie", "Straße" -> "strasse"), combining marks are dropped (so decomposed and
/// precomposed input agree), apostrophes are removed ("d'Amélie" -> "damelie"), every other ASCII
/// punctuation or whitespace run becomes one space, and leading/trailing spaces are trimmed.
/// Returns nullopt if `s` is not valid UTF-8.
std::optional<std::string> normalize_text(std::string_view s);

/// MovieLens moves leading articles to the end ("Matrix, The"). Returns the title with the article
/// moved back to the front ("The Matrix"), or nullopt if the title has no trailing article.
/// Only the main title is considered: in "Amélie (Fabuleux destin d'Amélie Poulain, Le)" the
/// parenthesised alternative title is left alone.
std::optional<std::string> article_first_title(std::string_view title);

/// Number of Unicode code points in valid UTF-8 `s`, or nullopt if `s` is not valid UTF-8.
std::optional<std::size_t> utf8_length(std::string_view s);

class SearchIndex {
 public:
  SearchIndex() = default;
  explicit SearchIndex(const Catalog& catalog);

  /// Items whose normalized title (or its article-first form) contains the normalized query.
  /// Prefix matches come first, then substring matches; within each group, more popular first
  /// (catalog popularity_rank ascending). At most `limit` results. `query` must be valid UTF-8.
  std::vector<ItemIdx> search(std::string_view query, std::size_t limit) const;

 private:
  struct Entry {
    std::string title;      // normalized title
    std::string alt_title;  // normalized article-first title, or empty
    std::uint32_t rank = 0;
  };
  std::vector<Entry> entries_;  // indexed by ItemIdx
};

}  // namespace recs::service
