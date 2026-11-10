// libFuzzer: text normalization, article handling and catalog search on arbitrary bytes.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz_common.hpp"
#include "search.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view s(reinterpret_cast<const char*>(data), size);
  const auto norm = recs::service::normalize_text(s);
  const auto len = recs::service::utf8_length(s);
  if (norm.has_value() != len.has_value()) __builtin_trap();  // both accept exactly valid UTF-8
  if (norm) {
    // Normalization is idempotent and never leaves leading, trailing or double spaces.
    if (recs::service::normalize_text(*norm) != norm) __builtin_trap();
    if (!norm->empty() && (norm->front() == ' ' || norm->back() == ' ')) __builtin_trap();
    if (norm->find("  ") != std::string::npos) __builtin_trap();
  }
  // Moving the article drops the ", " separator, so the result is always shorter.
  if (const auto alt = recs::service::article_first_title(s); alt && alt->size() >= s.size()) __builtin_trap();
  const auto bundle = recs::fuzz::harness().holder.get();
  for (auto i : bundle->search.search(s, 50)) {
    if (i >= bundle->n_items()) __builtin_trap();
  }
  return 0;
}
