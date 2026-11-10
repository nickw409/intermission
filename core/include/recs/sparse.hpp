// Sparse matrix construction and transposition. Workstream A owns the implementation.
// FROZEN after Wave 0.
#pragma once

#include <stdexcept>
#include <vector>

#include "recs/types.hpp"

namespace recs {

class SparseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Builds a CSR from unordered triplets: sorts by (row, col). Throws SparseError on a duplicate
/// (row, col) or an index out of range.
Csr csr_from_triplets(std::uint32_t n_rows, std::uint32_t n_cols, std::vector<Triplet> triplets);

/// Transpose (CSR <-> CSC). transpose(transpose(m)) == m for every valid m.
Csr transpose(const Csr& m);

/// Throws SparseError if any invariant documented on Csr is violated.
void validate(const Csr& m);

}  // namespace recs
