// Internal building blocks of the implicit ALS model (als.cpp), exposed so the gate tests can
// drive training one half-step at a time and compare each solve against a dense reference.
// Not part of the public librecs API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "recs/types.hpp"

namespace recs::als_detail {

/// Hyperparameters (configs/als.json). Defaults match that file.
struct Params {
  std::uint32_t k = 64;               // number of latent factors
  double lambda = 0.05;               // L2 regularisation (plain lambda * I, not scaled by counts)
  double alpha = 40.0;                // confidence c_ui = 1 + alpha for interactions
  int iters = 15;                     // full iterations (one item half-step + one user half-step)
  float interaction_threshold = 3.5f; // a rating >= this is an interaction
  double init_sd = 0.01;              // initial factors ~ N(0, init_sd^2)
};

/// Parses and validates a config; missing keys take the defaults, unknown keys or out-of-range
/// values throw std::invalid_argument.
Params parse_config(const Config& cfg);
Config to_config(const Params& p);

/// The interaction pattern (values are all 1): rows = users and rows = items.
struct Interactions {
  Csr by_user;
  Csr by_item;
};
Interactions build_interactions(const Dataset& train, float threshold);

/// Sorted, de-duplicated items the ratings count as interactions (any rating >= threshold).
/// Throws std::invalid_argument for an item >= n_items.
std::vector<std::uint32_t> interacted_items(std::span<const Rating> ratings, float threshold,
                                            std::uint32_t n_items);

/// G = F^T F (k x k, row-major, double) for a row-major float matrix F with n_rows rows.
/// Summed in fixed-size row blocks, then over blocks in order, so the result does not depend on
/// the number of threads.
std::vector<double> gram(std::span<const float> F, std::size_t n_rows, std::size_t k);

/// The normal equations of one row (user or item) with the other side F fixed:
///   A = G + alpha * sum_{j in rows} f_j f_j^T      (G = F^T F; lambda is NOT added here)
///   b = (1 + alpha) * sum_{j in rows} f_j
/// A is row-major k x k (both triangles filled), b has k entries; both double.
struct System {
  std::vector<double> A;
  std::vector<double> b;
};
System build_system(std::span<const double> G, std::span<const float> F, std::size_t k,
                    std::span<const std::uint32_t> rows, double alpha);

/// The closed-form ALS solve x = (A + lambda I)^{-1} b via solve_spd (Cholesky in double).
/// Training and fold-in both call exactly this.
std::vector<double> solve_row(std::span<const double> G, std::span<const float> F, std::size_t k,
                              std::span<const std::uint32_t> rows, double alpha, double lambda);

/// One half-step: recomputes every row r of `target` (pattern.n_rows x k) from the fixed side F
/// (pattern.n_cols x k), using the pattern row r as that row's interactions. Rows run in parallel
/// with a static OpenMP schedule; each row's result depends only on its own inputs.
void half_step(const Csr& pattern, std::span<const float> F, std::size_t k, double alpha,
               double lambda, std::vector<float>& target);

/// The full implicit objective over EVERY (user, item) pair, in double:
///   sum_{u,i} c_ui (p_ui - x_u.y_i)^2 + lambda (sum_u |x_u|^2 + sum_i |y_i|^2)
/// computed as trace(G_X G_Y) + sum over interactions of [(1+alpha)(1 - s)^2 - s^2]
/// + lambda (trace G_X + trace G_Y).
double objective(const Interactions& data, std::span<const float> X, std::span<const float> Y,
                 std::size_t k, double alpha, double lambda);

/// Per-item contributions to the score y_item . x_u, following Hu, Koren & Volinsky (2008) §5:
///   x_u = W b with W = (A + lambda I)^{-1}, so y_i . x_u = sum_j (1 + alpha) y_i^T W y_j.
/// Returns one value per entry of `rows` (the user's interactions), in the same order; they sum
/// to y_item . x_u up to rounding.
std::vector<double> contributions(std::span<const double> G, std::span<const float> F, std::size_t k,
                                  std::span<const std::uint32_t> rows, double alpha, double lambda,
                                  std::uint32_t item);

}  // namespace recs::als_detail
