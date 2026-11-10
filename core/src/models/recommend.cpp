// recommend(): the shared top-k used by every model, the evaluator and recs-model.
//
// 1. Score every item with the model (one float per item).
// 2. Mask: items in `exclude` and every item the user has rated get -infinity. Out-of-range
//    exclude entries are ignored (e.g. a movie the current artifact does not know).
// 3. Candidates are the items whose score is not -infinity ("cannot score" or masked).
// 4. Partial selection: std::nth_element puts the k best candidates first in O(n), then only those
//    k are sorted, in O(k log k) — instead of sorting the whole catalog in O(n log n).
//
// The order is a strict total order — score descending, then ItemIdx ascending — so ties are
// broken the same way on every run and every platform, and the result equals the first k entries
// of a full sort. A NaN score is a model bug (the Model contract forbids it) and throws
// std::logic_error rather than producing an arbitrary order.
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "recs/model.hpp"

namespace recs {

std::vector<Scored> recommend(const Model& m, const UserState& u, std::size_t k,
                              std::span<const ItemIdx> exclude) {
  const std::uint32_t n = m.n_items();
  if (k == 0 || n == 0) return {};
  std::vector<float> scores(n);
  m.score(u, scores);

  constexpr float kNegInf = -std::numeric_limits<float>::infinity();
  for (ItemIdx i : exclude) {
    if (i < n) scores[i] = kNegInf;
  }
  for (const Rating& r : u.ratings) {
    if (r.item < n) scores[r.item] = kNegInf;
  }

  std::vector<Scored> cand;
  cand.reserve(n);
  for (ItemIdx i = 0; i < n; ++i) {
    const float s = scores[i];
    if (std::isnan(s)) {
      throw std::logic_error(std::string(m.name()) + ": score() produced NaN for item " + std::to_string(i));
    }
    if (s != kNegInf) cand.push_back({i, s});
  }

  const auto better = [](const Scored& a, const Scored& b) {
    return a.score != b.score ? a.score > b.score : a.item < b.item;
  };
  const std::size_t take = std::min(k, cand.size());
  const auto mid = cand.begin() + static_cast<std::ptrdiff_t>(take);
  if (take < cand.size()) std::nth_element(cand.begin(), mid, cand.end(), better);
  cand.resize(take);
  std::sort(cand.begin(), cand.end(), better);
  return cand;
}

}  // namespace recs
