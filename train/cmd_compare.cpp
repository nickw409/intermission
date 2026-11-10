// STUB (Wave 0). Owner: workstream C.
#include <stdexcept>

#include "commands.hpp"

namespace recs::cli {
void add_compare_command(CLI::App& app) {
  auto* cmd = app.add_subcommand("compare", "compare (not implemented yet)");
  cmd->callback([] { throw std::runtime_error("compare is not implemented yet"); });
}
}  // namespace recs::cli
