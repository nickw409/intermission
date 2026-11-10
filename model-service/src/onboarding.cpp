// Onboarding picks: popular movies that cover many genres, for a new user's first ratings.
//
// Algorithm (greedy, deterministic)
// ---------------------------------
// Candidates are the rated, non-excluded movies in popularity order (most rated first). We keep a
// count c_g of how many picks so far carry genre g, and repeat n times:
//
//   1. Look at the W = max(2n, 10) most popular candidates not yet picked (a sliding window, so
//      every pick stays near the top of the popularity list).
//   2. Give each one the overlap  o(i) = (Σ_{g ∈ G_i} c_g) / |G_i|,  the average number of earlier
//      picks sharing each of its genres. A movie with no genres listed gets o(i) = the number of
//      genre-less picks so far.
//   3. Pick the lowest overlap; ties go to the more popular movie.
//
// The first pick is therefore the most popular movie, and later picks favour genres not yet shown
// while never reaching far down the popularity list. This is a simple form of maximal-marginal-
// relevance re-ranking (Carbonell & Goldstein, "The use of MMR, diversity-based reranking for
// reordering documents and producing summaries", SIGIR 1998), with popularity as relevance and
// genre overlap as redundancy.
//
// Cost: O(n · W · genres) — at most 100 · 200 · ~5 operations per request.
#include "onboarding.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace recs::service {

std::vector<ItemIdx> onboarding_picks(const Catalog& catalog, const std::vector<ItemIdx>& by_popularity,
                                      const std::unordered_set<ItemIdx>& exclude, std::size_t n) {
  const std::size_t window = std::max<std::size_t>(2 * n, 10);
  // The candidates any pick can reach: n picks, each from the next `window` unpicked candidates.
  std::vector<ItemIdx> cand;
  for (ItemIdx i : by_popularity) {
    if (cand.size() >= n + window) break;
    if (i < catalog.rating_count.size() && catalog.rating_count[i] > 0 && !exclude.contains(i)) cand.push_back(i);
  }

  std::map<std::string, std::size_t, std::less<>> genre_count;
  std::size_t genreless = 0;
  std::vector<bool> taken(cand.size(), false);
  std::vector<ItemIdx> picks;
  while (picks.size() < n) {
    std::size_t best = cand.size();
    double best_overlap = 0.0;
    std::size_t seen = 0;
    for (std::size_t c = 0; c < cand.size() && seen < window; ++c) {
      if (taken[c]) continue;
      ++seen;
      const auto& genres = catalog.movies[cand[c]].genres;
      double overlap = static_cast<double>(genreless);
      if (!genres.empty()) {
        double sum = 0.0;
        for (const auto& g : genres) {
          const auto it = genre_count.find(g);
          if (it != genre_count.end()) sum += static_cast<double>(it->second);
        }
        overlap = sum / static_cast<double>(genres.size());
      }
      if (best == cand.size() || overlap < best_overlap) {  // strict: ties keep the more popular
        best = c;
        best_overlap = overlap;
      }
    }
    if (best == cand.size()) break;  // no candidates left
    taken[best] = true;
    const ItemIdx item = cand[best];
    picks.push_back(item);
    const auto& genres = catalog.movies[item].genres;
    if (genres.empty()) ++genreless;
    for (const auto& g : genres) ++genre_count[g];
  }
  return picks;
}

}  // namespace recs::service
