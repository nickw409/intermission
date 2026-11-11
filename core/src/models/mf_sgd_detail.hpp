// Internals of the MF-SGD model (core/src/models/mf_sgd.cpp), exposed so the tests can check the
// math directly: the objective, its analytic gradient, and the single-rating SGD step that
// training actually runs. Not part of the frozen public interface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "recs/rng.hpp"
#include "recs/types.hpp"

namespace recs::mf_detail {

/// Every trainable parameter of biased MF, stored as T. Row-major factor matrices: P is
/// n_users x k, Q is n_items x k. mu is fixed (not trained) and always double.
template <class T>
struct BasicParams {
  std::uint32_t k = 0;
  double mu = 0.0;
  std::vector<T> bu;  // n_users
  std::vector<T> bi;  // n_items
  std::vector<T> P;   // n_users * k
  std::vector<T> Q;   // n_items * k
};
/// Double precision: the reference the gradient gates check, and training with precision
/// "double".
using Params = BasicParams<double>;
/// Single precision: what fit() trains with precision "float" (the default; half the memory
/// traffic, see mf_sgd.cpp "Speed").
using FloatParams = BasicParams<float>;

/// mu + b_u + b_i + p_u . q_i, accumulated in double (RMSE measurement, not the SGD step).
template <class T>
double predict(const BasicParams<T>& m, std::uint32_t u, std::uint32_t i);

/// J = sum over ratings (u, i, r) of
///       1/2 (r - predict(u, i))^2 + 1/2 lambda (b_u^2 + b_i^2 + |p_u|^2 + |q_i|^2).
/// The regulariser sits inside the sum (once per rating), which is the objective SGD descends.
double objective(const Params& m, std::span<const Triplet> ratings, double lambda);

/// Analytic dJ/dtheta, laid out like Params (k and mu copied, mu is not differentiated).
/// Built by summing the same per-rating gradient that sgd_step() applies.
Params gradient(const Params& m, std::span<const Triplet> ratings, double lambda);

/// One SGD step on one rating: theta <- theta - lr * d(per-rating loss)/d(theta), with every
/// partial derivative taken at the old parameter values (simultaneous update).
/// The arithmetic is done in T (lr and lambda rounded to T): for Params this is the double-precision
/// reference step; for FloatParams the same formula in float.
template <class T>
void sgd_step(BasicParams<T>& m, const Triplet& rating, double lr, double lambda);

/// A training rating as fit() stores it: 12 bytes (Triplet is 24). fit() shuffles these records
/// in place and reads them sequentially, rather than gathering Triplets through an index.
struct SgdRating {
  std::uint32_t row = 0;  // user
  std::uint32_t col = 0;  // item
  float value = 0.0f;
};
static_assert(sizeof(SgdRating) == 12);

/// One epoch: sgd_step() on every rating of `in_order`, front to back, with the same arithmetic
/// (results are bit-identical to that loop). While updating rating j it prefetches the
/// parameter rows of rating j + kPrefetchAhead, so the random reads of P and Q overlap with the
/// arithmetic instead of stalling it (the loop is memory-latency bound on ml-32m).
inline constexpr std::size_t kPrefetchAhead = 8;
template <class T>
void sgd_epoch(BasicParams<T>& m, std::span<const SgdRating> in_order, double lr, double lambda);

/// The order in which training visits the ratings, one permutation of [0, n) per epoch.
/// Stream: a single Rng(mix_seed(seed, 2)) for the whole run; each next() applies one more
/// Fisher-Yates shuffle to the previous epoch's permutation (starting from 0, 1, ..., n-1), so every
/// epoch gets a fresh order and the whole sequence is a function of the seed alone. (The brief
/// suggested a fresh Rng(seed ^ epoch) per epoch; one stream is equivalent in effect and avoids
/// correlated seeds for small epoch numbers.)
///
/// fit() does not use this class: it shuffles the rating records themselves with RecordShuffler,
/// which runs the same swaps on the same stream, so after each epoch records[j] equals
/// ratings[order[j]] (tested). Shuffling in place lets the epoch read the ratings sequentially
/// instead of chasing an index into a large array. EpochShuffler stays as the reference order.
class EpochShuffler {
 public:
  EpochShuffler(std::size_t n, std::uint64_t seed);
  const std::vector<std::uint32_t>& next();

 private:
  Rng rng_;
  std::vector<std::uint32_t> order_;
};

/// The epoch order applied to the records in place (see EpochShuffler): same seed derivation,
/// same Fisher-Yates swaps, one stream for the run. `stream` selects an independent stream per
/// DSGD cell, Rng(mix_seed(seed, 2, stream)); stream 0 is exactly EpochShuffler's.
class RecordShuffler {
 public:
  explicit RecordShuffler(std::uint64_t seed, std::uint64_t stream = 0);
  void next(std::span<SgdRating> records);

 private:
  Rng rng_;
};

// --- Parallel SGD over a block grid (DSGD, Gemulla et al. 2011; see mf_sgd.cpp) -------------

/// Splits items 0..n-1 into `b` groups with about equal total weight (weight = rating count):
/// visit them in a seeded random order and cut the running total into b equal parts. Returns
/// the group of each item. Deterministic in `rng`'s state.
std::vector<std::uint32_t> balanced_groups(std::span<const std::uint64_t> weight, std::uint32_t b, Rng& rng);

/// The ratings cut into b x b cells: cell (ub, ib) holds the ratings of users in user group ub on
/// items in item group ib, at index ub * b + ib, each initially in CSR order. Two cells that
/// differ in both coordinates share no user and no item, so SGD on them can run in parallel
/// without touching the same parameters.
struct BlockGrid {
  std::uint32_t b = 1;
  std::vector<std::uint32_t> user_group;        // n_users
  std::vector<std::uint32_t> item_group;        // n_items
  std::vector<std::vector<SgdRating>> cells;    // b * b
};

/// Groups from balanced_groups() with Rng(mix_seed(seed, 3, 0)) for users and
/// Rng(mix_seed(seed, 3, 1)) for items; `csr` must be in CSR order (user, then item ascending).
BlockGrid make_block_grid(std::span<const SgdRating> csr, std::uint32_t n_users, std::uint32_t n_items,
                          std::uint32_t b, std::uint64_t seed);

/// One DSGD epoch: b sub-epochs. The order of the b "strata" is a fresh seeded permutation
/// `offsets` each epoch (drawn from `stratum_rng`); in the sub-epoch with offset o, cell
/// (g, (g + o) mod b) for every g = 0..b-1 is shuffled with its own RecordShuffler (stream =
/// cell index) and then run through sgd_epoch, the b cells in parallel (static OpenMP schedule).
/// Each cell's work depends only on the parameters its own rows hold, so the result is the same
/// for any thread count. With b = 1 this is exactly one serial epoch (RecordShuffler stream 0).
template <class T>
void dsgd_epoch(BasicParams<T>& m, BlockGrid& grid, std::vector<RecordShuffler>& cell_shufflers, Rng& stratum_rng,
                double lr, double lambda);

}  // namespace recs::mf_detail
