// libFuzzer: manifest.json parsing and artifact verification (recs::load_artifact).
// The input replaces manifest.json in an otherwise valid artifact (artifact_template.hpp).
#include <cstddef>
#include <cstdint>
#include <exception>

#include "artifact_template.hpp"
#include "fuzz_util.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto& dir = recs::fuzz::template_artifact();
  recs::fuzz::write_file(dir / "manifest.json", recs::fuzz::as_view(data, size));
  try {
    const auto loaded = recs::load_artifact(dir);
    // Anything that loads must be internally consistent.
    for (const auto& [name, model] : loaded.models) {
      if (model->n_items() != loaded.catalog.items.size()) __builtin_trap();
    }
  } catch (const std::exception&) {
  }
  return 0;
}
