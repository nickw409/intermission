// recs-train ingest: parses a MovieLens directory and writes the binary dataset cache.
//
//   recs-train ingest --dataset ml-latest-small [--raw data/raw/ml-latest-small]
//                     [--cache data/cache/ml-latest-small.bin]
//
// Always parses the CSV files (it never trusts an existing cache) and then overwrites the cache,
// so running ingest is the way to refresh a cache after the raw files change.
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "commands.hpp"
#include "recs/dataset.hpp"

namespace recs::cli {

namespace {

struct IngestOptions {
  std::string dataset;
  std::string raw;
  std::string cache;
};

// Dataset names become path components, so keep them to a safe character set.
void check_dataset_name(const std::string& name) {
  const bool ok = !name.empty() && name != "." && name != ".." &&
                  name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
                      std::string::npos;
  if (!ok) throw std::runtime_error("invalid dataset name '" + name + "' (use letters, digits, '.', '_', '-')");
}

void run_ingest(const IngestOptions& opt) {
  check_dataset_name(opt.dataset);
  const fs::path raw = opt.raw.empty() ? fs::path("data/raw") / opt.dataset : fs::path(opt.raw);
  const fs::path cache = opt.cache.empty() ? fs::path("data/cache") / (opt.dataset + ".bin") : fs::path(opt.cache);

  using Clock = std::chrono::steady_clock;
  const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };

  std::cout << "ingest: parsing " << raw.string() << "\n" << std::flush;
  const auto t0 = Clock::now();
  const Dataset ds = load_movielens(raw, opt.dataset);
  const auto t1 = Clock::now();
  save_cache(ds, cache);
  const auto t2 = Clock::now();

  std::cout << "ingest: dataset " << ds.name << "\n"
            << "  ratings " << ds.n_ratings() << "\n"
            << "  movies  " << ds.n_items() << "\n"
            << "  users   " << ds.n_users() << "\n"
            << "  tags    " << ds.tags.size() << "\n"
            << "  genome  "
            << (ds.genome ? std::to_string(ds.genome->n_items) + " x " + std::to_string(ds.genome->n_tags) : "absent")
            << "\n"
            << "  parse " << seconds(t1 - t0) << " s, cache write " << seconds(t2 - t1) << " s\n"
            << "ingest: wrote " << cache.string() << " (" << fs::file_size(cache) << " bytes)\n";
}

}  // namespace

void add_ingest_command(CLI::App& app) {
  auto opt = std::make_shared<IngestOptions>();
  auto* cmd = app.add_subcommand("ingest", "Parse a MovieLens dataset and write its binary cache");
  cmd->add_option("--dataset", opt->dataset, "Dataset name, e.g. ml-latest-small or ml-32m")->required();
  cmd->add_option("--raw", opt->raw, "Directory with the MovieLens CSV files (default: data/raw/<dataset>)");
  cmd->add_option("--cache", opt->cache, "Cache file to write (default: data/cache/<dataset>.bin)");
  cmd->callback([opt] { run_ingest(*opt); });
}

}  // namespace recs::cli
