// recs-train compare: paired bootstrap of two runs' private per-user files.
//
//   recs-train compare --a <run_id> --b <run_id> [--private data/reports-private]
//                      [--out data/reports/comparisons] [--resamples 1000] [--seed 42]
//
// Reads <private>/<run_id>.per_user.json for both runs (written by eval --keep-per-user), which
// must share split_hash and evaluated_on. Writes <out>/<a>__vs__<b>.json
// (contracts/comparison.schema.json: aggregates only) and prints a table. A difference is called
// significant only when the paired 95% CI of the per-user deltas excludes zero (spec: Statistical
// honesty).
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "commands.hpp"
#include "recs/eval.hpp"

namespace recs::cli {
namespace {

namespace fs = std::filesystem;

struct CompareArgs {
  std::string a, b;
  std::string private_dir = "data/reports-private";
  std::string out = "data/reports/comparisons";
  std::size_t resamples = 1000;
  std::uint64_t seed = 42;
};

nlohmann::json read_per_user(const fs::path& dir, const std::string& run_id) {
  if (!std::regex_match(run_id, std::regex("^[A-Za-z0-9_-]{1,100}$"))) {
    throw std::invalid_argument("invalid run id '" + run_id + "'");
  }
  const fs::path p = dir / (run_id + ".per_user.json");
  std::ifstream in(p);
  if (!in) throw std::runtime_error("cannot read " + p.string() + " (run eval with --keep-per-user)");
  return nlohmann::json::parse(in);
}

void run_compare(const CompareArgs& args) {
  const auto a = read_per_user(args.private_dir, args.a);
  const auto b = read_per_user(args.private_dir, args.b);
  const auto cmp = compare_runs(a, b, args.resamples, args.seed);

  fs::create_directories(args.out);
  const fs::path path = fs::path(args.out) / (args.a + "__vs__" + args.b + ".json");
  const fs::path tmp = fs::path(args.out) / ("." + path.filename().string() + ".tmp-" + std::to_string(::getpid()));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << cmp.dump(2) << "\n";
    if (!out) throw std::runtime_error("cannot write " + tmp.string());
  }
  fs::rename(tmp, path);

  std::cout << "a = " << args.a << " (" << a["model"].get<std::string>() << ")\n"
            << "b = " << args.b << " (" << b["model"].get<std::string>() << ")\n"
            << "evaluated on " << cmp["evaluated_on"].get<std::string>() << ", " << args.resamples
            << " paired resamples, seed " << args.seed << "\n\n"
            << std::left << std::setw(14) << "metric" << std::right << std::setw(8) << "users" << std::setw(12)
            << "a - b" << std::setw(26) << "95% CI" << "  significant\n";
  for (const auto& [name, m] : cmp["metrics"].items()) {
    std::ostringstream ci;
    ci << std::fixed << std::setprecision(4) << "[" << m["ci95"][0].get<double>() << ", " << m["ci95"][1].get<double>() << "]";
    std::cout << std::left << std::setw(14) << name << std::right << std::setw(8) << m["users"].get<std::size_t>()
              << std::setw(12) << std::fixed << std::setprecision(4) << m["mean_delta"].get<double>() << std::setw(26)
              << ci.str() << "  " << (m["excludes_zero"].get<bool>() ? "yes" : "no") << "\n";
  }
  std::cout << "\ncomparison: " << path.string() << "\n";
}

}  // namespace

void add_compare_command(CLI::App& app) {
  auto args = std::make_shared<CompareArgs>();
  auto* cmd = app.add_subcommand("compare", "Paired bootstrap comparison of two evaluated runs");
  cmd->add_option("--a", args->a, "Run id A")->required();
  cmd->add_option("--b", args->b, "Run id B")->required();
  cmd->add_option("--private", args->private_dir, "Directory holding <run_id>.per_user.json files");
  cmd->add_option("--out", args->out, "Output directory for the comparison JSON");
  cmd->add_option("--resamples", args->resamples, "Bootstrap resamples")->check(CLI::PositiveNumber);
  cmd->add_option("--seed", args->seed, "Bootstrap seed");
  cmd->callback([args] { run_compare(*args); });
}

}  // namespace recs::cli
