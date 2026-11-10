// Offline evaluation and run reports (spec: Evaluation protocol, Run report).
// Workstream C owns the implementation (core/src/report/). FROZEN after Wave 0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "recs/metrics.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"
#include "recs/types.hpp"

namespace recs {

namespace fs = std::filesystem;

struct EvalOptions {
  std::vector<std::size_t> ks{1, 5, 10, 20, 50};  // per_k curves; 10 and 50 must be present
  std::size_t bootstrap_resamples = 1000;
  std::uint64_t bootstrap_seed = 42;
  std::uint32_t cold_train_ratings_lt = 20;  // cold-start slice: fewer than this many train ratings
  std::size_t parity_users = 500;          // fold-in parity sample size
  std::uint64_t parity_seed = 42;
  bool keep_per_user = false;  // also write the private per-user file (for paired bootstraps)
};

/// One entry per evaluated user. Ranking-metric vectors are empty when n_relevant == 0 (those
/// users are excluded from ranking-metric means but still count for pop rank and coverage).
struct PerUserResult {
  UserIdx user = 0;
  std::uint32_t n_train = 0;
  std::uint32_t n_relevant = 0;
  std::vector<double> ndcg_at;       // aligned with EvalOptions::ks
  std::vector<double> recall_at;     // aligned with EvalOptions::ks
  std::vector<double> precision_at;  // aligned with EvalOptions::ks
  double map10 = 0.0;
  double mean_pop_rank10 = 0.0;  // over the user's top-10 list (all users)
  // Rating task (only when the model predicts ratings).
  double sq_err = 0.0, abs_err = 0.0;
  std::uint32_t n_rated = 0;
};

struct EvalResult {
  std::vector<PerUserResult> per_user;  // every evaluated user, ascending UserIdx
  std::size_t users_without_relevant = 0;
  double coverage10 = 0.0;              // over every evaluated user's top-10
  bool has_rating_task = false;         // model predicts ratings
  std::optional<double> foldin_jaccard10;  // when the model supports_trained_scoring()
  double eval_seconds = 0.0;
};

/// Rating-task baselines (spec: global mean, user bias, item bias, user+item bias), fit on
/// `train` with the same damping as MF fold-in, evaluated on `heldout` for `users`.
struct RatingBaselineResult {
  std::string name;  // global_mean | user_bias | item_bias | user_item_bias
  std::vector<double> sq_err, abs_err, count;  // per user, aligned with `users`
};
std::vector<RatingBaselineResult> evaluate_rating_baselines(
    const Dataset& train, std::span<const std::vector<Rating>> heldout,
    std::span<const UserIdx> users);

/// Scores every user in `users` through m.fold_in(train ratings) and recommend(k = max ks),
/// excluding the user's training items, and compares with `heldout[user]`. Users may be scored in
/// parallel, but every reduction runs serially in ascending user order (determinism).
EvalResult evaluate(const Model& m, const Dataset& train,
                    std::span<const std::vector<Rating>> heldout, std::span<const UserIdx> users,
                    const EvalOptions& opt);

struct ReportHeader {
  std::string run_id;
  std::string git_sha;
  std::string dataset;
  const Split* split = nullptr;     // for split_hash, split config, cold_item_share
  std::string evaluated_on;         // "test" or "validation"
  std::string model;
  Config config;
  nlohmann::json seeds;             // every seed used: split, model, bootstrap, parity
  double train_seconds = 0.0;
  std::optional<std::uint64_t> peak_rss_bytes;
  std::vector<std::pair<int, double>> training_curve;
  std::string training_curve_metric;
  const std::vector<RatingBaselineResult>* rating_baselines = nullptr;  // null → JSON null
};

/// Builds a report that validates against contracts/report.schema.json (schema_version 1).
nlohmann::json make_report(const ReportHeader& h, const EvalResult& r, const EvalOptions& opt);

/// Writes reports_dir/<run_id>.json (2-space indent, keys sorted, trailing newline).
fs::path write_report(const nlohmann::json& report, const fs::path& reports_dir);

/// Private per-user file (contracts/per_user.schema.json) → private_dir/<run_id>.per_user.json.
/// private_dir must not be served (default data/reports-private).
fs::path write_per_user(const ReportHeader& h, const EvalResult& r, const EvalOptions& opt,
                        const fs::path& private_dir);

/// Paired bootstrap of two per-user files (contracts/comparison.schema.json). Throws
/// std::invalid_argument if split_hash or evaluated_on differ.
nlohmann::json compare_runs(const nlohmann::json& per_user_a, const nlohmann::json& per_user_b,
                            std::size_t resamples = 1000, std::uint64_t seed = 42);

/// "<UTC yyyy-mm-ddTHH-MM-SSZ>_<model>[_<suffix>]".
std::string make_run_id(std::string_view model, std::string_view suffix = "");

/// Peak resident set size of this process (VmHWM), if available.
std::optional<std::uint64_t> peak_rss_bytes();

}  // namespace recs
