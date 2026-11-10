// Ranking and rating metrics (spec: Evaluation protocol). Workstream B owns the implementation.
// FROZEN after Wave 0.
//
// Conventions:
//  - `ranked` is the recommendation list in rank order (training items already excluded).
//  - `heldout` is the user's held-out ratings. An item is relevant iff its rating >= 4.0.
//  - Graded gain = rating - 3.5 for relevant items, 0 otherwise. Discount = 1 / log2(rank + 1),
//    rank starting at 1. IDCG uses the relevant gains sorted descending, truncated to k.
//  - If the user has no relevant held-out items every ranking metric returns 0.0; the evaluator
//    excludes such users from ranking-metric means (and reports how many were excluded).
//  - Only the first k entries of `ranked` are used; if ranked.size() < k the missing ranks count
//    as misses.
//  - recall@k = hits / min(k, n_relevant); precision@k = hits / k;
//    AP@k = (1 / min(k, n_relevant)) * sum over hit ranks i <= k of precision@i.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "recs/types.hpp"

namespace recs {

inline constexpr float kRelevanceThreshold = 4.0f;
inline constexpr float kGainOffset = 3.5f;

std::size_t count_relevant(std::span<const Rating> heldout);
double ndcg_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k);
double recall_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k);
double precision_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k);
double average_precision_at_k(std::span<const ItemIdx> ranked, std::span<const Rating> heldout, std::size_t k);

/// |top-k(a) ∩ top-k(b)| / |top-k(a) ∪ top-k(b)|; 1.0 if both are empty.
double jaccard_at_k(std::span<const ItemIdx> a, std::span<const ItemIdx> b, std::size_t k);

/// Distinct items across every list's first k entries / n_items.
double catalog_coverage(const std::vector<std::vector<ItemIdx>>& lists, std::uint32_t n_items, std::size_t k);

/// Popularity rank per item from the number of ratings in `by_item` (rank 1 = most rated; ties
/// broken by lower ItemIdx).
std::vector<std::uint32_t> popularity_ranks(const Csr& by_item);

/// Mean popularity rank over every list's first k entries.
double mean_popularity_rank(const std::vector<std::vector<ItemIdx>>& lists,
                            const std::vector<std::uint32_t>& ranks, std::size_t k);

/// ROC AUC of `scores` (one per item) for separating `positives` from every other item not in
/// `exclude` (ties count 1/2). Used by the ALS planted-structure gate. Returns 0.5 if either class
/// is empty.
double roc_auc(std::span<const float> scores, std::span<const ItemIdx> positives,
               std::span<const ItemIdx> exclude);

struct RatingErrors {
  double rmse = 0.0;
  double mae = 0.0;
  std::size_t n = 0;
};
/// predicted[i] is the prediction for actual[i].value. Accumulates in double.
RatingErrors rating_errors(std::span<const float> predicted, std::span<const Rating> actual);

}  // namespace recs
