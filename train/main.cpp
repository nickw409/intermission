// recs-train: batch job. Reads files and writes files; never talks to a running service.
#include <exception>
#include <iostream>

#include "commands.hpp"

int main(int argc, char** argv) {
  CLI::App app{"recs-train: ingest, train, evaluate and retrain Intermission models"};
  app.require_subcommand(1);
  recs::cli::add_ingest_command(app);
  recs::cli::add_train_command(app);
  recs::cli::add_eval_command(app);
  recs::cli::add_retrain_command(app);
  recs::cli::add_compare_command(app);
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  } catch (const std::exception& e) {
    std::cerr << "recs-train: error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
