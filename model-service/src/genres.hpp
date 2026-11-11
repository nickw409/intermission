// The MovieLens genre set (contracts/model.yaml: Genre) as bits of a 32-bit mask.
//
// Each catalog movie gets a precomputed mask of its genres (Bundle::genre_mask), and a request's
// `genres` list becomes one mask, so "does movie i have at least one listed genre" is a single AND:
// (genre_mask[i] & wanted) != 0. No strings are compared per candidate.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace recs::service {

using GenreMask = std::uint32_t;

/// The contract's Genre enum, in contract order; bit b of a GenreMask stands for kGenres[b].
inline constexpr std::array<std::string_view, 19> kGenres = {
    "Action",  "Adventure", "Animation", "Children", "Comedy",  "Crime",  "Documentary",
    "Drama",   "Fantasy",   "Film-Noir", "Horror",   "IMAX",    "Musical", "Mystery",
    "Romance", "Sci-Fi",    "Thriller",  "War",      "Western"};
static_assert(kGenres.size() <= 32, "GenreMask has 32 bits");

/// The bit for a genre name (exact, case-sensitive match), or nullopt if it is not in the enum.
inline std::optional<GenreMask> genre_bit(std::string_view name) {
  for (std::size_t b = 0; b < kGenres.size(); ++b) {
    if (kGenres[b] == name) return GenreMask{1} << b;
  }
  return std::nullopt;
}

}  // namespace recs::service
