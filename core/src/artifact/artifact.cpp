// STUB (Wave 0). Owner: workstream C. Replace with the real implementation.
#include "recs/artifact.hpp"
#include "util/todo.hpp"
namespace recs {
Catalog make_catalog(const Dataset&) { RECS_TODO("make_catalog"); }
fs::path write_artifact(const fs::path&, const std::string&, const Catalog&, const std::vector<const Model*>&, const std::string&, const std::string&, const std::string&) { RECS_TODO("write_artifact"); }
LoadedArtifact load_artifact(const fs::path&) { RECS_TODO("load_artifact"); }
void write_current(const fs::path&, const std::string&) { RECS_TODO("write_current"); }
std::optional<std::string> read_current(const fs::path&) { RECS_TODO("read_current"); }
}  // namespace recs
