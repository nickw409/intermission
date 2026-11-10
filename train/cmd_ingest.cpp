// STUB (Wave 0). Owner: workstream A.
#include <stdexcept>

#include "commands.hpp"

namespace recs::cli {
void add_ingest_command(CLI::App& app) {
  auto* cmd = app.add_subcommand("ingest", "ingest (not implemented yet)");
  cmd->callback([] { throw std::runtime_error("ingest is not implemented yet"); });
}
}  // namespace recs::cli
