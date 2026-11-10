// libFuzzer: the catalog reader (load_catalog, reached through recs::load_artifact).
// The input replaces catalog/catalog.bin in an otherwise valid artifact (artifact_template.hpp);
// the harness rewrites the manifest with the new file's SHA-256 so verification passes and the
// catalog parser runs.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "artifact_template.hpp"
#include "fuzz_util.hpp"

namespace {

const nlohmann::json& template_manifest() {
  static const nlohmann::json j = [] {
    std::ifstream in(recs::fuzz::template_artifact() / "manifest.json", std::ios::binary);
    return nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
  }();
  return j;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto& dir = recs::fuzz::template_artifact();
  const auto bytes = recs::fuzz::as_view(data, size);
  nlohmann::json man = template_manifest();
  man["files"]["catalog/catalog.bin"] = recs::sha256_hex(bytes);
  recs::fuzz::write_file(dir / "catalog" / "catalog.bin", bytes);
  recs::fuzz::write_file(dir / "manifest.json", man.dump(2));
  try {
    const auto loaded = recs::load_artifact(dir);
    const auto& c = loaded.catalog;
    const std::size_t n = c.items.size();
    if (c.movies.size() != n || c.rating_count.size() != n || c.mean_rating.size() != n ||
        c.popularity_rank.size() != n) {
      __builtin_trap();
    }
  } catch (const std::exception&) {
  }
  return 0;
}
