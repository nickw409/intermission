// recs-train train: fit models on the FULL dataset (no split) and publish an artifact.
//
//   recs-train train --dataset ml-latest-small --models pop,pop_count --default-model pop
//                    [--out data/artifacts] [--set-current] [--config-dir configs] [--seed 42]
//
// Each model's config is <config-dir>/<model>.<dataset>.tuned.json if present (hyperparameters
// chosen on that dataset's validation split), else <config-dir>/<model>.json, else the model
// defaults; the log names the file each model used. `recs-train eval` instead takes an explicit
// --config (tuning and the one-off test evaluation should name their config). The
// artifact version is the UTC time, e.g. 2026-11-09T21-14-03Z. With --set-current, CURRENT is
// pointed at the new version. Afterwards the 3 most recent versions and whatever CURRENT points
// to are kept; older versions are deleted (contracts/artifact.md rule 6).
#include "dataset_name.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "commands.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

#ifndef RECS_GIT_SHA
#define RECS_GIT_SHA ""
#endif

namespace recs::cli {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kKeepVersions = 3;

struct TrainArgs {
  std::string dataset;
  std::string models = "pop,pop_count";
  std::string default_model = "pop";
  std::string out = "data/artifacts";
  std::string config_dir = "configs";
  std::string data_dir = "data";
  std::uint64_t seed = 42;
  bool set_current = false;
};

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

Config model_config(const fs::path& dir, const std::string& name, const std::string& dataset) {
  fs::path p = dir / (name + "." + dataset + ".tuned.json");
  if (!fs::exists(p)) p = dir / (name + ".json");
  if (!fs::exists(p)) {
    std::cout << "config " << name << ": model defaults (no " << name << "." << dataset << ".tuned.json or "
              << name << ".json in " << dir.string() << ")\n";
    return Config::object();
  }
  std::cout << "config " << name << ": " << p.string() << "\n";
  std::ifstream in(p);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  return Config::parse(in);
}

std::string version_now() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H-%M-%SZ", &tm);
  return buf;
}

// Deletes all but the newest kKeepVersions artifact directories, never the CURRENT one. Version
// names are UTC timestamps, so name order is time order. Only directories holding a
// manifest.json are considered; .tmp-* directories are left to write_artifact.
void prune(const fs::path& root) {
  const auto current = read_current(root);
  std::vector<std::string> versions;
  for (const auto& e : fs::directory_iterator(root)) {
    const auto name = e.path().filename().string();
    if (e.is_directory() && name.front() != '.' && fs::exists(e.path() / "manifest.json")) versions.push_back(name);
  }
  std::sort(versions.begin(), versions.end());
  if (versions.size() <= kKeepVersions) return;
  for (std::size_t i = 0; i + kKeepVersions < versions.size(); ++i) {
    if (current && versions[i] == *current) continue;
    std::cout << "pruning old artifact " << versions[i] << "\n";
    fs::remove_all(root / versions[i]);
  }
}

void run_train(const TrainArgs& a) {
  check_dataset_name(a.dataset);
  const auto names = split_list(a.models);
  if (names.empty()) throw std::invalid_argument("--models is empty");
  if (std::find(names.begin(), names.end(), a.default_model) == names.end()) {
    throw std::invalid_argument("--default-model must be one of --models");
  }
  std::vector<std::unique_ptr<Model>> models;
  for (const auto& n : names) models.push_back(make_model(n));  // validates names first

  const fs::path data(a.data_dir);
  const Dataset ds = load_or_build(data / "raw" / a.dataset, data / "cache" / (a.dataset + ".bin"), a.dataset);
  std::cout << "dataset: " << ds.name << " (" << ds.n_users() << " users, " << ds.n_items() << " movies, "
            << ds.n_ratings() << " ratings)\n";

  TrainContext ctx;
  ctx.seed = a.seed;
  ctx.deterministic = true;
  std::vector<const Model*> ptrs;
  for (auto& m : models) {
    const auto t0 = std::chrono::steady_clock::now();
    m->fit(ds, model_config(a.config_dir, std::string(m->name()), a.dataset), ctx);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "fit " << m->name() << " in " << secs << " s, config " << m->config().dump() << "\n";
    ptrs.push_back(m.get());
  }

  const std::string sha = RECS_GIT_SHA;
  const auto dir = write_artifact(a.out, version_now(), make_catalog(ds), ptrs, a.default_model,
                                  sha.empty() ? "unknown" : sha, a.dataset);
  std::cout << "artifact: " << dir.string() << "\n";
  if (a.set_current) {
    write_current(a.out, dir.filename().string());
    std::cout << "CURRENT -> " << dir.filename().string() << "\n";
  }
  prune(a.out);
}

}  // namespace

void add_train_command(CLI::App& app) {
  auto args = std::make_shared<TrainArgs>();
  auto* cmd = app.add_subcommand("train", "Fit models on the full dataset and write an artifact");
  cmd->add_option("--dataset", args->dataset, "Dataset name (data/raw/<name>)")->required();
  cmd->add_option("--models", args->models, "Comma-separated model names");
  cmd->add_option("--default-model", args->default_model, "Model served by For you");
  cmd->add_option("--out", args->out, "Artifact root directory");
  cmd->add_option("--config-dir", args->config_dir, "Directory of <model>.json configs");
  cmd->add_option("--data-dir", args->data_dir, "Data directory (raw/ and cache/)");
  cmd->add_option("--seed", args->seed, "Model seed")->check(CLI::Range(std::uint64_t{0}, (std::uint64_t{1} << 53) - 1));
  cmd->add_flag("--set-current", args->set_current, "Point CURRENT at the new artifact");
  cmd->callback([args] { run_train(*args); });
}

}  // namespace recs::cli
