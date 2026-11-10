// libFuzzer: the RFC 4180 CSV reader (recs::parse_csv / CsvReader).
// Input layout: the first byte picks the expected field count (0 = any, else byte % 8 + 1); the
// rest is the CSV text. Malformed input must throw CsvError and nothing else.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "fuzz_util.hpp"
#include "recs/csv.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const std::optional<std::size_t> expected =
      data[0] == 0 ? std::nullopt : std::optional<std::size_t>(data[0] % 8 + 1);
  const std::string_view text = recs::fuzz::as_view(data + 1, size - 1);
  try {
    const auto rows = recs::parse_csv(text, "fuzz", expected);
    for (const auto& r : rows) {
      if (expected && r.size() != *expected) __builtin_trap();
      std::size_t bytes = 0;
      for (const auto& f : r) bytes += f.size();
      if (bytes > text.size()) __builtin_trap();  // fields come from the input
    }
  } catch (const recs::CsvError&) {
  }
  return 0;
}
