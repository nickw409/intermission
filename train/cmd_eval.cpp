// STUB (Wave 0). Owner: workstream C.
#include <stdexcept>

#include "commands.hpp"

namespace recs::cli {
void add_eval_command(CLI::App& app) {
  auto* cmd = app.add_subcommand("eval", "eval (not implemented yet)");
  cmd->callback([] { throw std::runtime_error("eval is not implemented yet"); });
}
}  // namespace recs::cli
