// Ranking and rating metrics (spec: Evaluation protocol; definitions frozen in recs/metrics.hpp).
//
// Notation for one user: `ranked` = r_1, r_2, ... (the model's list, training items excluded),
// `heldout` = the user's held-out ratings, rel(i) = [rating(i) >= 4.0], n_rel = #relevant items,
// and hit_j = rel(r_j) for the first occurrence of r_j (a repeated item is never a second hit).
// Only ranks j <= k count; if the list is shorter than k the missing ranks are misses.
//
//   gain(i)      = rating(i) - 3.5 if rel(i), else 0        (graded relevance)
//   DCG@k        = sum_{j<=k} gain(r_j) / log2(j + 1)
//   IDCG@k       = sum_{j<=min(k, n_rel)} g_(j) / log2(j + 1), g_(1) >= g_(2) >= ... the relevant
//                  gains sorted descending (the best possible ordering)
//   NDCG@k       = DCG@k / IDCG@k                             (Jarvelin & Kekalainen, 2002)
//   precision@k  = hits@k / k
//   recall@k     = hits@k / min(k, n_rel)    (the min keeps a perfect top-k at 1 when n_rel > k)
//   AP@k         = (1 / min(k, n_rel)) * sum_{j<=k, hit_j} precision@j
//
// Every ranking metric is 0 when n_rel = 0; the evaluator excludes those users from the means.
// k = 0 is rejected (precision@0 would be 0/0).
//
// ROC AUC is the Mann-Whitney U statistic normalised by P * N: the probability that a random
// positive scores above a random negative, ties counting 1/2 (Hanley & McNeil, 1982). It is
// computed from average ranks in O(n log n).
//
// All sums are accumulated in double.
#include "recs/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace recs {

namespace {

void require_k(std::size_t k, const char* who) {
  if (k == 0) throw std::invalid_argument(std::string(who) + ": k must be > 0");
}

// The relevant held-out items of one user, sorted by item for binary search, with their gains.
class RelevantSet {
 public:
  explicit RelevantSet(std::span<const Rating> heldout) {
    std::vector<ItemIdx> all_items;
    all_items.reserve(heldout.size());
    for (const auto& r : heldout) {
      all_items.push_back(r.item);
      if (r.value >= kRelevanceThreshold) {
        entries_.push_back({r.item, static_cast<double>(r.value) - static_cast<double>(kGainOffset)});
      }
    }
    std::sort(all_items.begin(), all_items.end());
    if (std::adjacent_find(all_items.begin(), all_items.end()) != all_items.end()) {
      throw std::invalid_argument("ranking metric: duplicate item in held-out ratings");
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.item < b.item; });
    used_.assign(entries_.size(), false);
  }

  std::size_t size() const { return entries_.size(); }

  // If `item` is relevant and not hit before, marks it and returns its gain; otherwise nullopt.
  std::optional<double> hit(ItemIdx item) {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), item,
                                     [](const Entry& e, ItemIdx i) { return e.item < i; });
    if (it == entries_.end() || it->item != item) return std::nullopt;
    const auto pos = static_cast<std::size_t>(it - entries_.begin());
    if (used_[pos]) return std::nullopt;
    used_[pos] = true;
    return it->gain;
  }

  std::vector<double> gains() const {
    std::vector<double> g;
    g.reserve(entries_.size());
    for (const auto& e : entries_) g.push_back(e.gain);
    return g;
  }

 private:
  struct Entry {
    ItemIdx item;
    double gain;
  };
  std::vector<Entry> entries_;
  std::vector<bool> used_;
};

double discount(std::size_t rank) { return 1.0 / std::log2(static_cast<double>(rank) + 1.0); }

std::size_t hits_at_k(std::span<const ItemIdx> ranked, RelevantSet& rel, std::size_t k) {
  std::size_t hits = 0;
  const std::size_t n = std::min(k, ranked.size());
  for (std::size_t j = 0; j < n; ++j) {
    if (rel.hit(ranked[j])) ++hits;
  }
  return hits;
}

}  // namespace

std::size_t count_relevant(std::span<const Rating> heldout) {
  return static_cast<std::size_t>(std::count_if(
      heldout.begin(), heldout.end(), [](const Rating& r) { return r.value >= kRelevanceThreshold; }));
}

double ndcg_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k) {
  require_k(k, "ndcg_at_k");
  RelevantSet rel(heldout);
  if (rel.size() == 0) return 0.0;

  double dcg = 0.0;
  const std::size_t n = std::min(k, ranked.size());
  for (std::size_t j = 0; j < n; ++j) {
    if (auto g = rel.hit(ranked[j])) dcg += *g * discount(j + 1);
  }

  auto gains = rel.gains();
  std::sort(gains.begin(), gains.end(), std::greater<>());
  double idcg = 0.0;
  for (std::size_t j = 0; j < std::min(k, gains.size()); ++j) idcg += gains[j] * discount(j + 1);
  return dcg / idcg;  // idcg > 0: every relevant gain is >= 0.5
}

double recall_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k) {
  require_k(k, "recall_at_k");
  RelevantSet rel(heldout);
  if (rel.size() == 0) return 0.0;
  const std::size_t hits = hits_at_k(ranked, rel, k);
  return static_cast<double>(hits) / static_cast<double>(std::min(k, rel.size()));
}

double precision_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k) {
  require_k(k, "precision_at_k");
  RelevantSet rel(heldout);
  if (rel.size() == 0) return 0.0;
  const std::size_t hits = hits_at_k(ranked, rel, k);
  return static_cast<double>(hits) / static_cast<double>(k);
}

double average_precision_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout,
                              std::size_t k) {
  require_k(k, "average_precision_at_k");
  RelevantSet rel(heldout);
  if (rel.size() == 0) return 0.0;
  double sum = 0.0;
  std::size_t hits = 0;
  const std::size_t n = std::min(k, ranked.size());
  for (std::size_t j = 0; j < n; ++j) {
    if (rel.hit(ranked[j])) {
      ++hits;
      sum += static_cast<double>(hits) / static_cast<double>(j + 1);  // precision@(j+1)
    }
  }
  return sum / static_cast<double>(std::min(k, rel.size()));
}

double jaccard_at_k(std::span<const ItemIdx> a, std::span<const ItemIdx> b, std::size_t k) {
  std::vector<ItemIdx> sa(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(std::min(k, a.size())));
  std::vector<ItemIdx> sb(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min(k, b.size())));
  std::sort(sa.begin(), sa.end());
  sa.erase(std::unique(sa.begin(), sa.end()), sa.end());
  std::sort(sb.begin(), sb.end());
  sb.erase(std::unique(sb.begin(), sb.end()), sb.end());
  if (sa.empty() && sb.empty()) return 1.0;
  std::vector<ItemIdx> inter;
  std::set_intersection(sa.begin(), sa.end(), sb.begin(), sb.end(), std::back_inserter(inter));
  const std::size_t uni = sa.size() + sb.size() - inter.size();
  return static_cast<double>(inter.size()) / static_cast<double>(uni);
}

double catalog_coverage(const std::vector<std::vector<ItemIdx>>& lists, std::uint32_t n_items,
                        std::size_t k) {
  if (n_items == 0) throw std::invalid_argument("catalog_coverage: n_items must be > 0");
  std::vector<bool> seen(n_items, false);
  std::size_t distinct = 0;
  for (const auto& list : lists) {
    const std::size_t n = std::min(k, list.size());
    for (std::size_t j = 0; j < n; ++j) {
      const ItemIdx i = list[j];
      if (i >= n_items) throw std::invalid_argument("catalog_coverage: item outside the catalog");
      if (!seen[i]) {
        seen[i] = true;
        ++distinct;
      }
    }
  }
  return static_cast<double>(distinct) / static_cast<double>(n_items);
}

std::vector<std::uint32_t> popularity_ranks(const Csr& by_item) {
  const std::uint32_t n = by_item.n_rows;
  std::vector<ItemIdx> order(n);
  std::iota(order.begin(), order.end(), ItemIdx{0});
  // Most rated first; equal counts keep the lower ItemIdx first.
  std::stable_sort(order.begin(), order.end(), [&](ItemIdx a, ItemIdx b) {
    return by_item.row_size(a) > by_item.row_size(b);
  });
  std::vector<std::uint32_t> ranks(n);
  for (std::uint32_t r = 0; r < n; ++r) ranks[order[r]] = r + 1;
  return ranks;
}

double mean_popularity_rank(const std::vector<std::vector<ItemIdx>>& lists,
                            const std::vector<std::uint32_t>& ranks, std::size_t k) {
  // Pooled over every entry (not a mean of per-list means).
  double sum = 0.0;
  std::size_t count = 0;
  for (const auto& list : lists) {
    const std::size_t n = std::min(k, list.size());
    for (std::size_t j = 0; j < n; ++j) {
      if (list[j] >= ranks.size()) {
        throw std::invalid_argument("mean_popularity_rank: item outside the rank table");
      }
      sum += static_cast<double>(ranks[list[j]]);
      ++count;
    }
  }
  return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

double roc_auc(std::span<const float> scores, std::span<const ItemIdx> positives,
               std::span<const ItemIdx> exclude) {
  enum : std::uint8_t { kNeg = 0, kPos = 1, kExcluded = 2 };
  const std::size_t n = scores.size();
  std::vector<std::uint8_t> label(n, kNeg);
  for (ItemIdx i : exclude) {
    if (i >= n) throw std::invalid_argument("roc_auc: excluded item out of range");
    label[i] = kExcluded;
  }
  for (ItemIdx i : positives) {
    if (i >= n) throw std::invalid_argument("roc_auc: positive item out of range");
    if (label[i] == kExcluded) throw std::invalid_argument("roc_auc: item is both positive and excluded");
    label[i] = kPos;
  }

  std::vector<std::pair<float, std::uint8_t>> kept;
  kept.reserve(n);
  std::size_t n_pos = 0, n_neg = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (label[i] == kExcluded) continue;
    if (std::isnan(scores[i])) throw std::invalid_argument("roc_auc: NaN score");
    kept.emplace_back(scores[i], label[i]);
    (label[i] == kPos ? n_pos : n_neg) += 1;
  }
  if (n_pos == 0 || n_neg == 0) return 0.5;

  std::sort(kept.begin(), kept.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  // Sum of the positives' 1-based ranks, tied scores sharing their average rank.
  double pos_rank_sum = 0.0;
  for (std::size_t lo = 0; lo < kept.size();) {
    std::size_t hi = lo;
    while (hi < kept.size() && kept[hi].first == kept[lo].first) ++hi;
    const double avg_rank = (static_cast<double>(lo + 1) + static_cast<double>(hi)) / 2.0;
    for (std::size_t j = lo; j < hi; ++j) {
      if (kept[j].second == kPos) pos_rank_sum += avg_rank;
    }
    lo = hi;
  }
  const double p = static_cast<double>(n_pos), q = static_cast<double>(n_neg);
  const double u = pos_rank_sum - p * (p + 1.0) / 2.0;  // Mann-Whitney U for the positives
  return u / (p * q);
}

RatingErrors rating_errors(std::span<const float> predicted, std::span<const Rating> actual) {
  if (predicted.size() != actual.size()) {
    throw std::invalid_argument("rating_errors: predicted and actual differ in size");
  }
  RatingErrors e;
  e.n = actual.size();
  if (e.n == 0) return e;
  double sq = 0.0, ab = 0.0;
  for (std::size_t i = 0; i < e.n; ++i) {
    const double d = static_cast<double>(predicted[i]) - static_cast<double>(actual[i].value);
    sq += d * d;
    ab += std::abs(d);
  }
  e.rmse = std::sqrt(sq / static_cast<double>(e.n));
  e.mae = ab / static_cast<double>(e.n);
  return e;
}

}  // namespace recs
