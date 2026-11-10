// STUB (Wave 0). Owner: workstream C.
#include <stdexcept>

#include "commands.hpp"

namespace recs::cli {
void add_train_command(CLI::App& app) {
  auto* cmd = app.add_subcommand("train", "train (not implemented yet)");
  cmd->callback([] { throw std::runtime_error("train is not implemented yet"); });
}
}  // namespace recs::cli
