// Internals of the MF-SGD model (core/src/models/mf_sgd.cpp), exposed so the tests can check the
// math directly: the objective, its analytic gradient, and the single-rating SGD step that
// training actually runs. Not part of the frozen public interface.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "recs/rng.hpp"
#include "recs/types.hpp"

namespace recs::mf_detail {

/// Every trainable parameter of biased MF, in double precision (training precision).
/// Row-major factor matrices: P is n_users x k, Q is n_items x k. mu is fixed (not trained).
struct Params {
  std::uint32_t k = 0;
  double mu = 0.0;
  std::vector<double> bu;  // n_users
  std::vector<double> bi;  // n_items
  std::vector<double> P;   // n_users * k
  std::vector<double> Q;   // n_items * k
};

/// mu + b_u + b_i + p_u . q_i
double predict(const Params& m, std::uint32_t u, std::uint32_t i);

/// J = sum over ratings (u, i, r) of
///       1/2 (r - predict(u, i))^2 + 1/2 lambda (b_u^2 + b_i^2 + |p_u|^2 + |q_i|^2).
/// The regulariser sits inside the sum (once per rating), which is the objective SGD descends.
double objective(const Params& m, std::span<const Triplet> ratings, double lambda);

/// Analytic dJ/dtheta, laid out like Params (k and mu copied, mu is not differentiated).
/// Built by summing the same per-rating gradient that sgd_step() applies.
Params gradient(const Params& m, std::span<const Triplet> ratings, double lambda);

/// One SGD step on one rating: theta <- theta - lr * d(per-rating loss)/d(theta), with every
/// partial derivative taken at the old parameter values (simultaneous update).
void sgd_step(Params& m, const Triplet& rating, double lr, double lambda);

/// The order in which training visits the ratings, one permutation of [0, n) per epoch.
/// Stream: a single Rng(mix_seed(seed, 2)) for the whole run; each next() applies one more
/// Fisher-Yates shuffle to the previous epoch's permutation (starting from 0, 1, ..., n-1), so every
/// epoch gets a fresh order and the whole sequence is a function of the seed alone. (The brief
/// suggested a fresh Rng(seed ^ epoch) per epoch; one stream is equivalent in effect and avoids
/// correlated seeds for small epoch numbers.)
class EpochShuffler {
 public:
  EpochShuffler(std::size_t n, std::uint64_t seed);
  const std::vector<std::uint32_t>& next();

 private:
  Rng rng_;
  std::vector<std::uint32_t> order_;
};

}  // namespace recs::mf_detail
