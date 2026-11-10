// libFuzzer: the binary dataset cache reader (recs::load_cache).
// The input is the cache body; the harness appends its SHA-256 so the parser behind the checksum
// is exercised (fuzz_util.hpp).
#include <cstddef>
#include <cstdint>
#include <exception>

#include "fuzz_util.hpp"
#include "recs/dataset.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto file = recs::fuzz::scratch_dir() / "ds.bin";
  recs::fuzz::write_file(file, recs::fuzz::sealed(recs::fuzz::as_view(data, size)));
  try {
    const recs::Dataset ds = recs::load_cache(file);
    // A cache that loads must hold a consistent rating matrix.
    if (ds.by_user.indptr.size() != static_cast<std::size_t>(ds.by_user.n_rows) + 1) __builtin_trap();
    if (ds.by_user.indptr.back() != ds.by_user.indices.size()) __builtin_trap();
  } catch (const std::exception&) {
  }
  return 0;
}
