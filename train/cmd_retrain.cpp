// STUB (Wave 0). Owner: workstream P.
#include <stdexcept>

#include "commands.hpp"

namespace recs::cli {
void add_retrain_command(CLI::App& app) {
  auto* cmd = app.add_subcommand("retrain", "retrain (not implemented yet)");
  cmd->callback([] { throw std::runtime_error("retrain is not implemented yet"); });
}
}  // namespace recs::cli
