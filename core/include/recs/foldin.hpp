// Shared dense linear algebra for fold-in and ALS (spec: k x k Cholesky in double precision).
// Implemented in the spine (core/src/foldin/solve.cpp). FROZEN after Wave 0.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace recs {

/// Solves (A + lambda * I) x = b for symmetric positive (semi)definite A (k x k, row-major),
/// using a Cholesky (LLT) factorization in double precision. Throws std::runtime_error if the
/// system is not positive definite.
std::vector<double> solve_spd(std::span<const double> A, std::span<const double> b, std::size_t k,
                              double lambda);

}  // namespace recs
