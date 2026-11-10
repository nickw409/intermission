// Validation of CSR row-offset arrays read from untrusted bytes (internal to librecs).
//
// A row-offset array `ptr` for n rows and nnz stored entries is well formed when
//   ptr.size() == n + 1,  ptr[0] == 0,  ptr[n] == nnz,  ptr[r] <= ptr[r + 1] for every r.
// Together these guarantee 0 <= ptr[r] <= ptr[r + 1] <= nnz, so every row range lies inside the
// entry arrays. Loaders must run this check over the whole array BEFORE walking any row: checking
// monotonicity row by row while walking is not enough, because an earlier row can claim entries
// past nnz (e.g. ptr = [0, 3, 1] with nnz = 1) and be read before the later decrease is seen
// (audit finding SA2-1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace recs::detail {

/// Returns a description of the first violated rule, or nullopt if `ptr` is well formed.
inline std::optional<std::string> check_row_offsets(std::span<const std::uint64_t> ptr, std::uint64_t n_rows,
                                                    std::uint64_t nnz) {
  if (ptr.size() != n_rows + 1) return "row offsets: size != n_rows + 1";
  if (ptr.front() != 0) return "row offsets: first offset != 0";
  if (ptr.back() != nnz) return "row offsets: last offset != nnz";
  for (std::size_t r = 0; r + 1 < ptr.size(); ++r) {
    if (ptr[r] > ptr[r + 1]) return "row offsets: decrease at row " + std::to_string(r);
  }
  return std::nullopt;
}

}  // namespace recs::detail
