// recs-train eval: split, fit on the training part, evaluate, write the run report.
//
//   recs-train eval --dataset ml-latest-small --model pop [--config configs/pop.json]
//                   [--on test|validation] [--split-seed 42] [--seed 42] [--bootstrap 1000]
//                   [--reports data/reports] [--private data/reports-private] [--keep-per-user]
//
// --on test fits on split.train (train + validation) and evaluates on the test set; models get no
// validation data then (their epochs/iterations come from the tuned config). --on validation fits
// on split.train_minus_val, passes the validation set to fit() for early stopping, and evaluates
// on it. The test set is evaluated once per final config (spec: Hyperparameter tuning).
#include "dataset_name.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../core/src/report/output_files.hpp"
#include "commands.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"

#ifndef RECS_GIT_SHA
#define RECS_GIT_SHA ""
#endif

namespace recs::cli {
namespace {

namespace fs = std::filesystem;

// Seeds are recorded in JSON reports, which must stay exact in every JSON reader (2^53 - 1).
constexpr std::uint64_t kMaxSeed = (std::uint64_t{1} << 53) - 1;

struct EvalArgs {
  std::string dataset;
  std::string model;
  std::string config;
  std::string on = "test";
  std::uint64_t split_seed = 42;
  std::uint64_t seed = 42;
  std::size_t bootstrap = 1000;
  std::string data_dir = "data";
  std::string reports = "data/reports";
  std::string private_dir = "data/reports-private";
  bool keep_per_user = false;
  bool rating_baselines = false;
};

Config read_config(const std::string& explicit_path, const std::string& model, const std::string& dataset,
                   const std::string& on) {
  fs::path path = explicit_path;
  if (path.empty()) {
    // eval never picks a tuned config by itself: tuning on validation must not feed on earlier
    // tuning results, and the one-off test evaluation should name its config. Say so, though.
    const fs::path tuned = fs::path("configs") / (model + "." + dataset + ".tuned.json");
    if (on == "test" && fs::exists(tuned)) {
      std::cout << "warning: " << tuned.string() << " exists but is not used; pass --config to evaluate it\n";
    }
    path = fs::path("configs") / (model + ".json");
    if (!fs::exists(path)) {
      std::cout << "config: none given and " << path << " not found; using model defaults\n";
      return Config::object();
    }
  }
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read config " + path.string());
  std::cout << "config: " << path.string() << "\n";
  return Config::parse(in);
}

std::string git_sha() {
  const std::string sha = RECS_GIT_SHA;
  return sha.empty() ? "unknown" : sha;
}

void print_estimate(const char* name, const nlohmann::json& e) {
  std::cout << "  " << std::left << std::setw(18) << name << std::fixed << std::setprecision(4)
            << e["mean"].get<double>() << "  [" << e["ci95"][0].get<double>() << ", "
            << e["ci95"][1].get<double>() << "]\n";
}

void run_eval(const EvalArgs& a) {
  check_dataset_name(a.dataset);
  // Everything that can fail cheaply is checked before the dataset is loaded.
  if (a.on != "test" && a.on != "validation") throw std::invalid_argument("--on must be test or validation");
  const Config cfg = read_config(a.config, a.model, a.dataset, a.on);
  auto model = make_model(a.model);
  const std::string run_id = make_run_id(a.model);
  fs::create_directories(a.reports);
  if (fs::exists(fs::path(a.reports) / (run_id + ".json"))) throw std::runtime_error("report " + run_id + " already exists");
  if (a.keep_per_user) {
    // Per-user files identify users and must never be served: --private may neither be nor
    // contain nor lie inside --reports (canonical paths, so `..` and symlinks are resolved).
    if (output::paths_nested(a.private_dir, a.reports)) {
      throw std::invalid_argument("--private and --reports must be separate directories (neither inside the other)");
    }
    fs::create_directories(a.private_dir);
  }

  const fs::path data(a.data_dir);
  const Dataset full = load_or_build(data / "raw" / a.dataset, data / "cache" / (a.dataset + ".bin"), a.dataset);
  std::cout << "dataset: " << full.name << " (" << full.n_users() << " users, " << full.n_items() << " movies, "
            << full.n_ratings() << " ratings)\n";

  SplitConfig sc;
  sc.seed = a.split_seed;
  const Split split = make_split(full, sc);
  const bool on_test = a.on == "test";
  const Dataset& train = on_test ? split.train : split.train_minus_val;
  const auto& heldout = on_test ? split.test : split.validation;

  TrainContext ctx;
  ctx.seed = a.seed;
  ctx.deterministic = true;
  ctx.validation = on_test ? nullptr : &split.validation;
  const auto t0 = std::chrono::steady_clock::now();
  model->fit(train, cfg, ctx);
  const double train_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  EvalOptions opt;
  opt.bootstrap_resamples = a.bootstrap;
  opt.bootstrap_seed = a.seed;
  opt.parity_seed = a.seed;
  opt.keep_per_user = a.keep_per_user;
  const EvalResult res = evaluate(*model, train, heldout, split.eval_users, opt);

  std::vector<RatingBaselineResult> baselines;
  const bool with_baselines = a.rating_baselines || model->predicts_ratings();
  if (with_baselines) baselines = evaluate_rating_baselines(train, heldout, split.eval_users);

  ReportHeader h;
  h.run_id = run_id;
  h.git_sha = git_sha();
  h.dataset = a.dataset;
  h.split = &split;
  h.evaluated_on = a.on;
  h.model = a.model;
  h.config = model->config();
  h.seeds = {{"split", a.split_seed}, {"model", a.seed}, {"bootstrap", opt.bootstrap_seed}, {"parity", opt.parity_seed}};
  h.train_seconds = train_seconds;
  h.training_curve = model->training_curve();
  h.training_curve_metric = model->training_curve_metric();
  h.rating_baselines = with_baselines ? &baselines : nullptr;
  h.peak_rss_bytes = peak_rss_bytes();

  const auto report = make_report(h, res, opt);
  // The per-user file is written first; if the report then fails, it is removed again, so a
  // report never exists without the per-user file it was asked to keep (and vice versa).
  fs::path per_user_path;
  if (a.keep_per_user) per_user_path = write_per_user(h, res, opt, a.private_dir);
  fs::path path;
  try {
    path = write_report(report, a.reports);
  } catch (...) {
    if (!per_user_path.empty()) {
      std::error_code ec;
      fs::remove(per_user_path, ec);
    }
    throw;
  }
  std::cout << "report: " << path.string() << "\n";
  if (a.keep_per_user) std::cout << "per-user: " << per_user_path.string() << "\n";

  const auto& m = report["metrics"];
  std::cout << "\n" << a.model << " on " << a.dataset << " (" << a.on << ", " << report["counts"]["users_evaluated"]
            << " users, " << report["counts"]["users_without_relevant"] << " without a relevant item)\n";
  for (const char* k : {"ndcg@10", "recall@10", "recall@50", "precision@10", "map@10", "mean_pop_rank@10"}) {
    print_estimate(k, m[k]);
  }
  std::cout << "  coverage@10         " << std::fixed << std::setprecision(4)
            << report["aggregates"]["coverage@10"].get<double>() << "\n";
  if (!report["slices"]["cold_users"]["ndcg@10"].is_null()) print_estimate("cold ndcg@10", report["slices"]["cold_users"]["ndcg@10"]);
  if (!report["rating_metrics"].is_null()) print_estimate("rmse", report["rating_metrics"]["rmse"]);
  if (!report["foldin_jaccard@10"].is_null()) {
    std::cout << "  foldin_jaccard@10   " << report["foldin_jaccard@10"].get<double>() << "\n";
  }
  std::cout << "  cold_item_share     " << report["cold_item_share"].get<double>() << "\n"
            << "  train " << std::setprecision(2) << train_seconds << " s, eval " << res.eval_seconds << " s\n";
}

}  // namespace

void add_eval_command(CLI::App& app) {
  auto args = std::make_shared<EvalArgs>();
  auto* cmd = app.add_subcommand("eval", "Split, fit, evaluate and write a run report");
  cmd->add_option("--dataset", args->dataset, "Dataset name (data/raw/<name>)")->required();
  cmd->add_option("--model", args->model, "Model name (pop, pop_count, knn, content, mf, als)")->required();
  cmd->add_option("--config", args->config, "Config JSON (default configs/<model>.json if present)");
  cmd->add_option("--on", args->on, "Held-out set: test or validation")->check(CLI::IsMember({"test", "validation"}));
  cmd->add_option("--split-seed", args->split_seed, "Split seed")->check(CLI::Range(std::uint64_t{0}, kMaxSeed));
  cmd->add_option("--seed", args->seed, "Model, bootstrap and parity seed")->check(CLI::Range(std::uint64_t{0}, kMaxSeed));
  cmd->add_option("--bootstrap", args->bootstrap, "Bootstrap resamples")->check(CLI::PositiveNumber);
  cmd->add_option("--data-dir", args->data_dir, "Data directory (raw/ and cache/)");
  cmd->add_option("--reports", args->reports, "Report output directory");
  cmd->add_option("--private", args->private_dir, "Private per-user output directory (never served)");
  cmd->add_flag("--keep-per-user", args->keep_per_user, "Also write the private per-user file");
  cmd->add_flag("--rating-baselines", args->rating_baselines, "Include rating baselines even if the model does not predict ratings");
  cmd->callback([args] { run_eval(*args); });
}

}  // namespace recs::cli
