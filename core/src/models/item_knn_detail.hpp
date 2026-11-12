// Item-KNN internals exposed for the gate tests (tests/core/models/item_knn/). Not part of the
// public librecs API: nothing outside item_knn.cpp and its tests should include this header.
#pragma once

#include <cstdint>
#include <vector>

#include "recs/model.hpp"
#include "recs/types.hpp"

namespace recs::item_knn_detail {

/// Hyperparameters (configs/knn.json). The defaults are the spec's top-N form, with M, lambda and
/// mean_damping chosen on the ml-latest-small validation split (ml-32m uses configs/knn.ml-32m.tuned.json).
struct Params {
  std::uint32_t neighbors = 10;    // M: neighbours kept per item
  double shrinkage = 1000.0;       // lambda in s * n / (n + lambda)
  std::uint32_t min_support = 20;  // minimum training ratings for an item to take part at all
  bool normalize = false;          // true = divide the score by sum |s'| (rating-prediction form)
  bool positive_only = true;       // keep only s' > 0 neighbours (false = rank by |s'|, negatives kept)
  double mean_damping = 20.0;      // beta: the scoring mean is (sum r + beta * global mean) / (n + beta)
};

/// Reads a knn config, filling defaults for missing keys. Throws std::invalid_argument on an
/// unknown key, a wrong type (including a non-bool normalize/positive_only), neighbors == 0,
/// min_support == 0, a negative/non-finite shrinkage, or a mean_damping outside [0, 1e6].
Params parse_config(const Config& cfg);
Config to_config(const Params& p);

/// One entry of an item's untruncated similarity row.
struct PairSimilarity {
  ItemIdx item = 0;           // the other item j
  std::uint32_t support = 0;  // n_ij: users who rated both items
  double raw = 0.0;           // s_ij, adjusted cosine over co-raters, clamped to [-1, 1]
  double shrunk = 0.0;        // s'_ij = s_ij * n_ij / (n_ij + lambda)
  bool operator==(const PairSimilarity&) const = default;
};

/// Every item's full similarity row before top-M truncation, computed by exactly the code fit()
/// uses. Indexed by ItemIdx; items below min_support get an empty row and never appear in another
/// item's row. Each row holds every eligible j != i with n_ij >= 1 (zero similarities included),
/// sorted by ItemIdx. `positive_only` does not filter here (it only affects the stored lists).
/// O(sum over users of n_u^2) time and O(n_items^2) output: tests only.
std::vector<std::vector<PairSimilarity>> full_similarity(const Dataset& train, const Config& cfg);

/// The stored (truncated) neighbour list of `item` in a fitted or loaded "knn" model, in stored
/// order: |s'| descending, then ItemIdx ascending. Throws std::invalid_argument if `m` is not the
/// item-KNN model and std::out_of_range for a bad item.
std::vector<Scored> neighbour_list(const Model& m, ItemIdx item);

}  // namespace recs::item_knn_detail
