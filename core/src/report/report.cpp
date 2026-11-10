// Run reports (contracts/report.schema.json), private per-user files
// (contracts/per_user.schema.json) and paired run comparisons (contracts/comparison.schema.json).
//
// Every number with a per-user decomposition is reported as a mean with a 95% percentile
// bootstrap CI over users (recs/stats.hpp, 1000 resamples, seeded). Scope:
//  - ranking metrics (NDCG, recall, precision, MAP and the per_k curves): users with at least one
//    relevant held-out item;
//  - mean_pop_rank@10: every evaluated user with a non-empty list;
//  - coverage@10: a single population number, no CI;
//  - RMSE / MAE: ratios of per-user sums (sqrt(Σ sq_err / Σ n) and Σ abs_err / Σ n), bootstrapped
//    by resampling users (recs::bootstrap_ratio_ci), so every held-out rating weighs the same.
//  - users without a held-out rating are not resampling units for RMSE / MAE (they add nothing).
// A metric with no contributing users is written as mean 0 with CI [0, 0] where the schema needs a
// number, and as null where the schema allows it (the cold-user slice).
//
// Determinism: the report is a pure function of its inputs apart from created_at and the timing /
// memory fields, which the header supplies. nlohmann::json keeps object keys sorted, and doubles
// are printed with the shortest round-trip representation, so equal inputs give equal bytes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <unistd.h>

#include "recs/eval.hpp"
#include "recs/stats.hpp"

namespace recs {
namespace {

const std::regex kRunIdPattern("^[A-Za-z0-9_-]{1,100}$");

std::string utc_now(const char* fmt) {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

double finite(double v, const char* what) {
  if (!std::isfinite(v)) throw std::logic_error(std::string("report: non-finite value for ") + what);
  return v;
}

nlohmann::json estimate_json(const Estimate& e, const char* what) {
  return {{"mean", finite(e.mean, what)}, {"ci95", {finite(e.lo, what), finite(e.hi, what)}}};
}

nlohmann::json mean_ci(const std::vector<double>& values, const EvalOptions& opt, const char* what) {
  if (values.empty()) return estimate_json({}, what);
  return estimate_json(bootstrap_ci(values, opt.bootstrap_resamples, opt.bootstrap_seed), what);
}

// Users without a held-out rating add nothing to either sum, so they are left out of the
// resampling units (otherwise a resample could draw only such users and have no ratio at all).
nlohmann::json rating_errors_json(const std::vector<double>& sq_all, const std::vector<double>& abs_all,
                                  const std::vector<double>& count_all, const EvalOptions& opt) {
  std::vector<double> sq, abs_err, count;
  double n = 0.0;
  for (std::size_t j = 0; j < count_all.size(); ++j) {
    if (count_all[j] <= 0.0) continue;
    sq.push_back(sq_all[j]);
    abs_err.push_back(abs_all[j]);
    count.push_back(count_all[j]);
    n += count_all[j];
  }
  nlohmann::json rmse = estimate_json({}, "rmse");
  nlohmann::json mae = estimate_json({}, "mae");
  if (n > 0.0) {
    rmse = estimate_json(bootstrap_ratio_ci(sq, count, opt.bootstrap_resamples, opt.bootstrap_seed, 0.95, true), "rmse");
    mae = estimate_json(bootstrap_ratio_ci(abs_err, count, opt.bootstrap_resamples, opt.bootstrap_seed, 0.95, false), "mae");
  }
  return {{"rmse", rmse}, {"mae", mae}, {"n", static_cast<std::uint64_t>(n)}};
}

std::size_t k_index(const EvalOptions& opt, std::size_t k) {
  const auto it = std::find(opt.ks.begin(), opt.ks.end(), k);
  if (it == opt.ks.end()) throw std::invalid_argument("EvalOptions::ks must contain " + std::to_string(k));
  return static_cast<std::size_t>(it - opt.ks.begin());
}

void check_header(const ReportHeader& h) {
  if (h.split == nullptr) throw std::invalid_argument("ReportHeader::split is null");
  if (h.evaluated_on != "test" && h.evaluated_on != "validation") {
    throw std::invalid_argument("ReportHeader::evaluated_on must be \"test\" or \"validation\"");
  }
  if (!std::regex_match(h.run_id, kRunIdPattern)) throw std::invalid_argument("invalid run_id '" + h.run_id + "'");
}

/// Writes `text` to dir/name via a temporary file and rename; refuses to replace an existing file.
fs::path write_new_file(const fs::path& dir, const std::string& name, const std::string& text) {
  fs::create_directories(dir);
  const fs::path final_path = dir / name;
  if (fs::exists(final_path)) throw std::runtime_error("refusing to overwrite " + final_path.string());
  const fs::path tmp = dir / ("." + name + ".tmp-" + std::to_string(::getpid()));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    if (!out) throw std::runtime_error("cannot write " + tmp.string());
  }
  fs::rename(tmp, final_path);
  return final_path;
}

// Values of one ranking metric, null for users without a relevant item.
nlohmann::json per_user_column(const EvalResult& r, std::size_t q, const std::vector<double> PerUserResult::*field) {
  nlohmann::json col = nlohmann::json::array();
  for (const auto& pu : r.per_user) {
    if (pu.n_relevant == 0) col.push_back(nullptr);
    else col.push_back((pu.*field)[q]);
  }
  return col;
}

}  // namespace

nlohmann::json make_report(const ReportHeader& h, const EvalResult& r, const EvalOptions& opt) {
  check_header(h);
  const std::size_t i10 = k_index(opt, 10);
  const std::size_t i50 = k_index(opt, 50);
  const Split& split = *h.split;
  const Dataset& trained_on = h.evaluated_on == "test" ? split.train : split.train_minus_val;

  // Per-user columns, ascending user order.
  std::vector<double> ndcg10, recall10, recall50, precision10, map10, pop_rank, cold_ndcg10;
  std::size_t cold_users = 0;
  for (const auto& pu : r.per_user) {
    const bool cold = pu.n_train < opt.cold_train_ratings_lt;
    if (cold) ++cold_users;
    if (!std::isnan(pu.mean_pop_rank10)) pop_rank.push_back(pu.mean_pop_rank10);
    if (pu.n_relevant == 0) continue;
    if (pu.ndcg_at.size() != opt.ks.size()) throw std::invalid_argument("make_report: per-user vectors do not match ks");
    ndcg10.push_back(pu.ndcg_at[i10]);
    recall10.push_back(pu.recall_at[i10]);
    recall50.push_back(pu.recall_at[i50]);
    precision10.push_back(pu.precision_at[i10]);
    map10.push_back(pu.map10);
    if (cold) cold_ndcg10.push_back(pu.ndcg_at[i10]);
  }

  nlohmann::json per_k = {{"ndcg", nlohmann::json::array()}, {"recall", nlohmann::json::array()},
                          {"precision", nlohmann::json::array()}};
  for (std::size_t q = 0; q < opt.ks.size(); ++q) {
    double s_ndcg = 0.0, s_recall = 0.0, s_precision = 0.0;
    std::size_t n = 0;
    for (const auto& pu : r.per_user) {
      if (pu.n_relevant == 0) continue;
      s_ndcg += pu.ndcg_at[q];
      s_recall += pu.recall_at[q];
      s_precision += pu.precision_at[q];
      ++n;
    }
    const double d = n == 0 ? 1.0 : static_cast<double>(n);
    const auto k = static_cast<std::uint64_t>(opt.ks[q]);
    per_k["ndcg"].push_back({k, s_ndcg / d});
    per_k["recall"].push_back({k, s_recall / d});
    per_k["precision"].push_back({k, s_precision / d});
  }

  nlohmann::json rating_metrics = nullptr;
  if (r.has_rating_task) {
    std::vector<double> sq, ab, cnt;
    double total = 0.0;
    for (const auto& pu : r.per_user) {
      sq.push_back(pu.sq_err);
      ab.push_back(pu.abs_err);
      cnt.push_back(pu.n_rated);
      total += pu.n_rated;
    }
    if (total > 0.0) rating_metrics = rating_errors_json(sq, ab, cnt, opt);
  }

  nlohmann::json baselines = nullptr;
  if (h.rating_baselines != nullptr) {
    baselines = nlohmann::json::object();
    for (const auto& b : *h.rating_baselines) baselines[b.name] = rating_errors_json(b.sq_err, b.abs_err, b.count, opt);
    for (const char* name : {"global_mean", "user_bias", "item_bias", "user_item_bias"}) {
      if (!baselines.contains(name)) throw std::invalid_argument(std::string("make_report: missing rating baseline ") + name);
    }
    if (baselines.size() != 4) throw std::invalid_argument("make_report: unknown rating baseline");
  }

  nlohmann::json curve = nlohmann::json::array();
  for (const auto& [it, v] : h.training_curve) curve.push_back({it, finite(v, "training_curve")});

  nlohmann::json foldin = nullptr;
  if (r.foldin_jaccard10) foldin = finite(*r.foldin_jaccard10, "foldin_jaccard@10");

  nlohmann::json rep;
  rep["schema_version"] = 1;
  rep["run_id"] = h.run_id;
  rep["created_at"] = utc_now("%Y-%m-%dT%H:%M:%SZ");
  rep["git_sha"] = h.git_sha;
  rep["dataset"] = h.dataset;
  rep["split_hash"] = split.hash;
  rep["split"] = {{"min_ratings", split.config.min_ratings}, {"test_fraction", split.config.test_fraction},
                  {"val_fraction", split.config.val_fraction}, {"test_min", split.config.test_min},
                  {"test_max", split.config.test_max},       {"seed", split.config.seed}};
  rep["evaluated_on"] = h.evaluated_on;
  rep["model"] = h.model;
  rep["config"] = h.config.is_null() ? Config::object() : h.config;
  rep["seeds"] = h.seeds.is_null() ? nlohmann::json::object() : h.seeds;
  rep["train_seconds"] = std::max(0.0, finite(h.train_seconds, "train_seconds"));
  rep["eval_seconds"] = std::max(0.0, finite(r.eval_seconds, "eval_seconds"));
  rep["peak_rss_bytes"] = h.peak_rss_bytes ? nlohmann::json(*h.peak_rss_bytes) : nlohmann::json(nullptr);
  rep["counts"] = {{"users_evaluated", r.per_user.size()},
                   {"users_without_relevant", r.users_without_relevant},
                   {"n_users", trained_on.n_users()},
                   {"n_items", trained_on.n_items()},
                   {"n_train_ratings", trained_on.n_ratings()}};
  rep["cold_item_share"] = finite(split.cold_item_share, "cold_item_share");
  rep["metrics"] = {{"ndcg@10", mean_ci(ndcg10, opt, "ndcg@10")},
                    {"recall@10", mean_ci(recall10, opt, "recall@10")},
                    {"recall@50", mean_ci(recall50, opt, "recall@50")},
                    {"precision@10", mean_ci(precision10, opt, "precision@10")},
                    {"map@10", mean_ci(map10, opt, "map@10")},
                    {"mean_pop_rank@10", mean_ci(pop_rank, opt, "mean_pop_rank@10")}};
  rep["aggregates"] = {{"coverage@10", finite(r.coverage10, "coverage@10")}};
  rep["per_k"] = per_k;
  rep["slices"] = {{"cold_users",
                    {{"train_ratings_lt", opt.cold_train_ratings_lt},
                     {"users", cold_users},
                     {"ndcg@10", cold_ndcg10.empty() ? nlohmann::json(nullptr) : mean_ci(cold_ndcg10, opt, "cold ndcg@10")}}}};
  rep["rating_metrics"] = rating_metrics;
  rep["rating_baselines"] = baselines;
  rep["training_curve"] = curve;
  rep["training_curve_metric"] = h.training_curve_metric;
  rep["foldin_jaccard@10"] = foldin;
  return rep;
}

fs::path write_report(const nlohmann::json& report, const fs::path& reports_dir) {
  const auto run_id = report.value("run_id", std::string());
  if (!std::regex_match(run_id, kRunIdPattern)) throw std::invalid_argument("write_report: invalid run_id '" + run_id + "'");
  return write_new_file(reports_dir, run_id + ".json", report.dump(2) + "\n");
}

fs::path write_per_user(const ReportHeader& h, const EvalResult& r, const EvalOptions& opt,
                        const fs::path& private_dir) {
  check_header(h);
  // Per-user results identify (dense) users: never write them where reports are served.
  for (const auto& part : fs::absolute(private_dir).lexically_normal()) {
    if (part == "reports") throw std::invalid_argument("write_per_user: refusing to write inside a reports directory");
  }
  const std::size_t i10 = k_index(opt, 10);
  const std::size_t i50 = k_index(opt, 50);

  nlohmann::json users = nlohmann::json::array(), n_relevant = nlohmann::json::array(), pop = nlohmann::json::array();
  nlohmann::json map10 = nlohmann::json::array();
  for (const auto& pu : r.per_user) {
    users.push_back(pu.user);
    n_relevant.push_back(pu.n_relevant);
    pop.push_back(std::isnan(pu.mean_pop_rank10) ? nlohmann::json(nullptr) : nlohmann::json(pu.mean_pop_rank10));
    map10.push_back(pu.n_relevant == 0 ? nlohmann::json(nullptr) : nlohmann::json(pu.map10));
  }
  nlohmann::json doc;
  doc["schema_version"] = 1;
  doc["run_id"] = h.run_id;
  doc["model"] = h.model;
  doc["dataset"] = h.dataset;
  doc["split_hash"] = h.split->hash;
  doc["evaluated_on"] = h.evaluated_on;
  doc["users"] = users;
  doc["n_relevant"] = n_relevant;
  doc["metrics"] = {{"ndcg@10", per_user_column(r, i10, &PerUserResult::ndcg_at)},
                    {"recall@10", per_user_column(r, i10, &PerUserResult::recall_at)},
                    {"recall@50", per_user_column(r, i50, &PerUserResult::recall_at)},
                    {"precision@10", per_user_column(r, i10, &PerUserResult::precision_at)},
                    {"map@10", map10},
                    {"pop_rank@10", pop}};
  if (r.has_rating_task) {
    nlohmann::json sq = nlohmann::json::array(), ab = nlohmann::json::array(), cnt = nlohmann::json::array();
    for (const auto& pu : r.per_user) {
      sq.push_back(pu.sq_err);
      ab.push_back(pu.abs_err);
      cnt.push_back(pu.n_rated);
    }
    doc["rating"] = {{"sq_err", sq}, {"abs_err", ab}, {"count", cnt}};
  } else {
    doc["rating"] = nullptr;
  }
  return write_new_file(private_dir, h.run_id + ".per_user.json", doc.dump() + "\n");
}

// compare_runs: a paired bootstrap on per-user deltas a - b over the users present in both files
// (for a ranking metric, those with a non-null value in both). "rmse" is compared when both runs
// have rating data, as the per-user RMSE sqrt(sq_err / count) over users with count > 0 in both;
// its mean_delta is therefore a mean of per-user RMSE differences, not the difference of the two
// global RMSEs.
nlohmann::json compare_runs(const nlohmann::json& a, const nlohmann::json& b, std::size_t resamples,
                            std::uint64_t seed) {
  try {
    for (const auto* doc : {&a, &b}) {
      if (doc->at("schema_version") != 1) throw std::invalid_argument("compare_runs: unknown per-user schema_version");
    }
    if (a.at("split_hash") != b.at("split_hash")) throw std::invalid_argument("compare_runs: split_hash differs");
    if (a.at("evaluated_on") != b.at("evaluated_on")) throw std::invalid_argument("compare_runs: evaluated_on differs");
    if (resamples == 0) throw std::invalid_argument("compare_runs: resamples must be > 0");

    const auto ua = a.at("users").get<std::vector<std::uint64_t>>();
    const auto ub = b.at("users").get<std::vector<std::uint64_t>>();
    std::unordered_map<std::uint64_t, std::size_t> pos_b;
    for (std::size_t j = 0; j < ub.size(); ++j) pos_b.emplace(ub[j], j);
    std::vector<std::pair<std::size_t, std::size_t>> common;  // (index in a, index in b), a's order
    for (std::size_t j = 0; j < ua.size(); ++j) {
      if (auto it = pos_b.find(ua[j]); it != pos_b.end()) common.emplace_back(j, it->second);
    }

    auto entry = [&](const std::vector<double>& va, const std::vector<double>& vb) {
      nlohmann::json e;
      e["users"] = va.size();
      if (va.empty()) {
        e["mean_delta"] = 0.0;
        e["ci95"] = {0.0, 0.0};
        e["excludes_zero"] = false;
      } else {
        const auto pc = paired_bootstrap(va, vb, resamples, seed);
        e["mean_delta"] = finite(pc.mean_delta, "mean_delta");
        e["ci95"] = {finite(pc.lo, "ci95"), finite(pc.hi, "ci95")};
        e["excludes_zero"] = pc.excludes_zero;
      }
      return e;
    };

    nlohmann::json metrics = nlohmann::json::object();
    for (const char* name : {"ndcg@10", "recall@10", "recall@50", "precision@10", "map@10", "pop_rank@10"}) {
      const auto& ca = a.at("metrics").at(name);
      const auto& cb = b.at("metrics").at(name);
      if (ca.size() != ua.size() || cb.size() != ub.size()) throw std::invalid_argument("compare_runs: column length mismatch");
      std::vector<double> va, vb;
      for (const auto& [ia, ib] : common) {
        if (ca[ia].is_null() || cb[ib].is_null()) continue;
        va.push_back(ca[ia].get<double>());
        vb.push_back(cb[ib].get<double>());
      }
      metrics[name] = entry(va, vb);
    }

    const bool ra = a.contains("rating") && a["rating"].is_object();
    const bool rb = b.contains("rating") && b["rating"].is_object();
    if (ra && rb) {
      const auto& xa = a["rating"];
      const auto& xb = b["rating"];
      std::vector<double> va, vb;
      for (const auto& [ia, ib] : common) {
        const double na = xa.at("count").at(ia).get<double>();
        const double nb = xb.at("count").at(ib).get<double>();
        if (na <= 0.0 || nb <= 0.0) continue;
        va.push_back(std::sqrt(xa.at("sq_err").at(ia).get<double>() / na));
        vb.push_back(std::sqrt(xb.at("sq_err").at(ib).get<double>() / nb));
      }
      metrics["rmse"] = entry(va, vb);
    }

    nlohmann::json out;
    out["schema_version"] = 1;
    out["a"] = {{"run_id", a.at("run_id")}, {"model", a.at("model")}};
    out["b"] = {{"run_id", b.at("run_id")}, {"model", b.at("model")}};
    out["split_hash"] = a.at("split_hash");
    out["evaluated_on"] = a.at("evaluated_on");
    out["resamples"] = resamples;
    out["seed"] = seed;
    out["metrics"] = metrics;
    return out;
  } catch (const nlohmann::json::exception& e) {
    throw std::invalid_argument(std::string("compare_runs: malformed per-user file: ") + e.what());
  }
}

std::string make_run_id(std::string_view model, std::string_view suffix) {
  std::string id = utc_now("%Y-%m-%dT%H-%M-%SZ") + "_" + std::string(model);
  if (!suffix.empty()) id += "_" + std::string(suffix);
  if (model.empty() || !std::regex_match(id, kRunIdPattern)) {
    throw std::invalid_argument("make_run_id: model and suffix may only use [A-Za-z0-9_-]");
  }
  return id;
}

std::optional<std::uint64_t> peak_rss_bytes() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      try {
        return static_cast<std::uint64_t>(std::stoull(line.substr(6))) * 1024u;  // value is in kB
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

}  // namespace recs
