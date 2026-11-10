// Temporal train / validation / test split (spec: Evaluation protocol -> Split).
// Workstream B owns the implementation. FROZEN after Wave 0: the protocol may only change with
// the owner's sign-off and a report schema_version bump.
//
// Users with fewer than min_ratings ratings are not evaluated: all their ratings go to `train`
// and `train_minus_val` (so website users with few ratings still shape the model), and they get
// no validation or test ratings.
// Per user with >= min_ratings ratings:
//   1. Sort ratings by (timestamp, tie_key) where tie_key = mix_seed(seed, user, item), so equal
//      timestamps are ordered deterministically by the seed.
//   2. n_test = clamp(round_half_up(test_fraction * n), test_min, test_max). The last n_test
//      ratings are the test set.
//   3. Of the remaining n_train ratings, n_val = clamp(round_half_up(val_fraction * n_train),
//      val_min, val_max) last ones are the validation set.
//   4. Test items that never appear in `train` are kept (cold items) and counted in cold_item_share.
// All three Datasets keep the full user and item index spaces of the input, so indices agree.
// The split is for evaluation only: served artifacts are fit on the FULL dataset.
//
// Hash: SHA-256 over, in order: the config as compact JSON with sorted keys
// ({"min_ratings":..,"seed":..,"test_fraction":..,"test_max":..,"test_min":..,"val_fraction":..,
// "val_max":..,"val_min":..} as produced by nlohmann::json::dump()), then for each user in index
// order and each of the user's ratings in item order the bytes "<user>,<item>,<set>\n" where set is
// one of t (train-minus-val), v (validation), x (test). Ratings' values/timestamps are covered by
// the dataset itself; the hash identifies the assignment.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "recs/types.hpp"

namespace recs {

struct SplitConfig {
  std::uint32_t min_ratings = 10;
  double test_fraction = 0.2;
  double val_fraction = 0.2;
  std::uint32_t test_min = 1;
  std::uint32_t test_max = 10;
  std::uint32_t val_min = 1;
  std::uint32_t val_max = 10;
  std::uint64_t seed = 42;
};

struct Split {
  SplitConfig config;
  std::string hash;  // hex SHA-256 over the config and every (user, item, set) assignment
  Dataset train;            // train + validation ratings: fit final models on this, evaluate on test
  Dataset train_minus_val;  // train without validation: fit for tuning / early stopping
  std::vector<std::vector<Rating>> validation;  // indexed by UserIdx; empty for non-evaluated users
  std::vector<std::vector<Rating>> test;        // indexed by UserIdx; empty for non-evaluated users
  std::vector<UserIdx> eval_users;              // users with >= min_ratings, ascending
  double cold_item_share = 0.0;  // share of test ratings whose item has no rating in `train`
};

Split make_split(const Dataset& full, const SplitConfig& cfg);

}  // namespace recs
