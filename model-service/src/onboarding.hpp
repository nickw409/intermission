// Onboarding picks: popular, genre-diverse movies for a new user to rate.
#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "recs/artifact.hpp"

namespace recs::service {

/// Greedy diverse selection from the most-rated movies.
/// `by_popularity` lists every item most-rated first. Items in `exclude` and items nobody has rated
/// are skipped. See onboarding.cpp for the algorithm. Returns at most n distinct items.
std::vector<ItemIdx> onboarding_picks(const Catalog& catalog, const std::vector<ItemIdx>& by_popularity,
                                      const std::unordered_set<ItemIdx>& exclude, std::size_t n);

}  // namespace recs::service
