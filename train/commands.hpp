// recs-train subcommands. Wiring file (orchestrator). Each command lives in its own cmd_*.cpp:
// ingest -> workstream A, train/eval/compare -> workstream C, retrain -> workstream P.
#pragma once
#include <CLI/CLI.hpp>

namespace recs::cli {
void add_ingest_command(CLI::App& app);
void add_train_command(CLI::App& app);
void add_eval_command(CLI::App& app);
void add_retrain_command(CLI::App& app);
void add_compare_command(CLI::App& app);
}  // namespace recs::cli
