// Gates 8 and 9 (C1): end-to-end popularity evaluation on the tiny fixture (and on ml-latest-small
// when it is present), and the Phase 0 determinism gate. Reports are written to
// build/<preset>/test-reports/ so CI can validate them with the Node contract validator.
#include <filesystem>
#include <string>

#include <omp.h>

#include <catch2/catch_test_macros.hpp>

#include "../report/schema_check.hpp"
#include "../report/test_util.hpp"
#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"

namespace fs = std::filesystem;

namespace {

// build/<preset>/test-reports: the nearest ancestor of the test binary holding a CMakeCache.txt
// (independent of the working directory), else a directory under the system temp dir.
fs::path test_reports_dir() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    for (fs::path p = exe.parent_path(); !p.empty(); p = p.parent_path()) {
      if (fs::exists(p / "CMakeCache.txt")) return p / "test-reports";
      if (p == p.parent_path()) break;
    }
  }
  return fs::temp_directory_path() / "recs-test-reports";
}

struct Run {
  nlohmann::json report;
  recs::EvalResult result;
};

Run run_pop(const recs::Dataset& full, const std::string& model_name, const std::string& run_id) {
  const recs::SplitConfig sc;  // protocol defaults, seed 42
  const auto split = recs::make_split(full, sc);
  auto model = recs::make_model(model_name);
  recs::TrainContext ctx;
  ctx.seed = 42;
  model->fit(split.train, recs::Config::object(), ctx);
  recs::EvalOptions opt;
  Run out;
  out.result = recs::evaluate(*model, split.train, split.test, split.eval_users, opt);
  recs::ReportHeader h;
  h.run_id = run_id;
  h.git_sha = "test";
  h.dataset = full.name;
  h.split = &split;
  h.evaluated_on = "test";
  h.model = model_name;
  h.config = model->config();
  h.seeds = {{"split", sc.seed}, {"model", ctx.seed}, {"bootstrap", opt.bootstrap_seed}, {"parity", opt.parity_seed}};
  h.train_seconds = 0.0;
  h.peak_rss_bytes = recs::peak_rss_bytes();
  out.report = recs::make_report(h, out.result, opt);
  return out;
}

std::string stable(nlohmann::json rep) {
  // Exactly the volatile fields listed in contracts/report.schema.json.
  for (const char* k : {"run_id", "created_at", "train_seconds", "eval_seconds", "peak_rss_bytes"}) {
    REQUIRE(rep.contains(k));
    rep.erase(k);
  }
  return rep.dump(2);
}

// Writes the report to build/<preset>/test-reports/ (replacing an earlier test run's file) so CI
// can validate real output with the Node contract validator.
void write_test_report(const nlohmann::json& rep) {
  const auto dir = test_reports_dir();
  fs::create_directories(dir);
  fs::remove(dir / (rep["run_id"].get<std::string>() + ".json"));
  CHECK(fs::exists(recs::write_report(rep, dir)));
}

void require_valid(const nlohmann::json& doc) {
  const auto errs = recs::testing::SchemaChecker::from_contract("report.schema.json").errors(doc);
  for (const auto& e : errs) UNSCOPED_INFO(e);
  REQUIRE(errs.empty());
}

}  // namespace

TEST_CASE("tiny: load -> split -> fit pop -> evaluate -> report", "[c1][integration]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  for (const std::string name : {"pop", "pop_count"}) {
    INFO(name);
    const auto run = run_pop(ds, name, "tiny_" + name);
    require_valid(run.report);
    const auto& rep = run.report;
    CHECK(rep["model"] == name);
    CHECK(rep["dataset"] == "tiny");
    CHECK(rep["counts"]["n_items"] == 30);
    CHECK(rep["counts"]["n_users"] == 20);
    CHECK(rep["counts"]["users_evaluated"] == 18);  // users 19 and 20 have < 10 ratings
    CHECK(rep["cold_item_share"].get<double>() > 0.0);  // user 7's movie 29
    CHECK(rep["foldin_jaccard@10"].is_null());
    CHECK(rep["rating_metrics"].is_null());
    const double ndcg = rep["metrics"]["ndcg@10"]["mean"].get<double>();
    CHECK(ndcg >= 0.0);
    CHECK(ndcg <= 1.0);
    write_test_report(rep);
  }
}

TEST_CASE("determinism gate: same seeds give byte-identical reports, 1 vs 4 threads", "[c1][integration][determinism]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  omp_set_num_threads(1);
  const auto a = stable(run_pop(ds, "pop", "run_a").report);
  omp_set_num_threads(4);
  const auto b = stable(run_pop(ds, "pop", "run_b").report);
  const auto c = stable(run_pop(ds, "pop", "run_c").report);
  omp_set_num_threads(2);
  CHECK(a == b);
  CHECK(b == c);
}

TEST_CASE("ml-latest-small: popularity end to end (skipped without the data)", "[c1][integration][ml-small]") {
  const auto raw = recs::testing::data_dir() / "raw" / "ml-latest-small";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-latest-small not downloaded");
  const auto ds = recs::load_movielens(raw, "ml-latest-small");
  const auto run = run_pop(ds, "pop", "ml-latest-small_pop_test");
  require_valid(run.report);
  write_test_report(run.report);
  CHECK(run.report["counts"]["n_users"] == 610);
  CHECK(run.report["counts"]["n_items"] == 9742);
  const auto& ndcg = run.report["metrics"]["ndcg@10"];
  CHECK(ndcg["mean"].get<double>() > 0.0);
  CHECK(ndcg["ci95"][0].get<double>() <= ndcg["mean"].get<double>());
  CHECK(ndcg["ci95"][1].get<double>() >= ndcg["mean"].get<double>());
}
