// Sparse matrix construction and transposition (spine-owned, see recs/sparse.hpp).
//
// CSR stores, for each row r, the column indices indices[indptr[r] .. indptr[r+1]) in strictly
// increasing order, with matching values and timestamps. The transpose (CSC of the same matrix)
// is built with a counting sort over columns: count entries per column, prefix-sum into indptr,
// then scatter. Scattering rows in increasing order keeps each output row sorted, so no extra sort
// is needed and the result is deterministic.
#include "recs/sparse.hpp"

#include <algorithm>
#include <string>

namespace recs {

Csr csr_from_triplets(std::uint32_t n_rows, std::uint32_t n_cols, std::vector<Triplet> triplets) {
  for (const auto& t : triplets) {
    if (t.row >= n_rows || t.col >= n_cols) {
      throw SparseError("triplet (" + std::to_string(t.row) + ", " + std::to_string(t.col) +
                        ") out of range for " + std::to_string(n_rows) + " x " + std::to_string(n_cols));
    }
  }
  std::sort(triplets.begin(), triplets.end(), [](const Triplet& a, const Triplet& b) {
    return a.row != b.row ? a.row < b.row : a.col < b.col;
  });
  Csr m;
  m.n_rows = n_rows;
  m.n_cols = n_cols;
  m.indptr.assign(static_cast<std::size_t>(n_rows) + 1, 0);
  m.indices.reserve(triplets.size());
  m.values.reserve(triplets.size());
  m.ts.reserve(triplets.size());
  for (std::size_t i = 0; i < triplets.size(); ++i) {
    const auto& t = triplets[i];
    if (i > 0 && triplets[i - 1].row == t.row && triplets[i - 1].col == t.col) {
      throw SparseError("duplicate entry (" + std::to_string(t.row) + ", " + std::to_string(t.col) + ")");
    }
    ++m.indptr[t.row + 1];
    m.indices.push_back(t.col);
    m.values.push_back(t.value);
    m.ts.push_back(t.ts);
  }
  for (std::uint32_t r = 0; r < n_rows; ++r) m.indptr[r + 1] += m.indptr[r];
  return m;
}

Csr transpose(const Csr& m) {
  Csr t;
  t.n_rows = m.n_cols;
  t.n_cols = m.n_rows;
  t.indptr.assign(static_cast<std::size_t>(m.n_cols) + 1, 0);
  for (std::uint32_t c : m.indices) ++t.indptr[c + 1];
  for (std::uint32_t c = 0; c < m.n_cols; ++c) t.indptr[c + 1] += t.indptr[c];
  const std::uint64_t nnz = m.nnz();
  t.indices.resize(nnz);
  t.values.resize(nnz);
  t.ts.resize(nnz);
  std::vector<std::uint64_t> next(t.indptr.begin(), t.indptr.end() - 1);
  for (std::uint32_t r = 0; r < m.n_rows; ++r) {
    for (std::uint64_t k = m.indptr[r]; k < m.indptr[r + 1]; ++k) {
      const std::uint64_t dst = next[m.indices[k]]++;
      t.indices[dst] = r;
      t.values[dst] = m.values[k];
      t.ts[dst] = m.ts[k];
    }
  }
  return t;
}

void validate(const Csr& m) {
  if (m.indptr.size() != static_cast<std::size_t>(m.n_rows) + 1) throw SparseError("indptr size != n_rows + 1");
  if (m.indptr.front() != 0) throw SparseError("indptr[0] != 0");
  if (m.indptr.back() != m.indices.size()) throw SparseError("indptr.back() != nnz");
  if (m.values.size() != m.indices.size() || m.ts.size() != m.indices.size()) {
    throw SparseError("values/ts size != nnz");
  }
  for (std::uint32_t r = 0; r < m.n_rows; ++r) {
    if (m.indptr[r + 1] < m.indptr[r]) throw SparseError("indptr decreases at row " + std::to_string(r));
    for (std::uint64_t k = m.indptr[r]; k < m.indptr[r + 1]; ++k) {
      if (m.indices[k] >= m.n_cols) throw SparseError("column out of range in row " + std::to_string(r));
      if (k > m.indptr[r] && m.indices[k] <= m.indices[k - 1]) {
        throw SparseError("columns not strictly increasing in row " + std::to_string(r));
      }
    }
  }
}

}  // namespace recs
