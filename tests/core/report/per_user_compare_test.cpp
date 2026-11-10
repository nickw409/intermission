// Gate 6 (C1): private per-user files, compare_runs, and the report writer helpers.
#include <cmath>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fake_models.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "schema_check.hpp"
#include "test_util.hpp"

using Catch::Approx;
using recs::testing::FixedScoreModel;
using recs::testing::TempDir;

namespace {

struct Run {
  recs::Dataset train = recs::make_dataset("pu", 3, 6, {{0, 0, 4.f, 1}, {1, 1, 3.f, 1}, {2, 0, 5.f, 1}, {2, 1, 2.f, 1}});
  std::vector<std::vector<recs::Rating>> heldout = {{{3, 4.5f, 9}, {1, 2.0f, 9}}, {{2, 3.0f, 9}}, {{2, 4.0f, 9}}};
  std::vector<recs::UserIdx> users = {0, 1, 2};
  recs::Split split;
  Run() {
    split.hash = std::string(64, 'c');
    split.train = train;
    split.train_minus_val = train;
  }
  recs::ReportHeader header(const std::string& run_id, const std::string& model) const {
    recs::ReportHeader h;
    h.run_id = run_id;
    h.dataset = "pu";
    h.split = &split;
    h.evaluated_on = "test";
    h.model = model;
    h.config = recs::Config::object();
    return h;
  }
};

nlohmann::json load(const recs::fs::path& p) { return nlohmann::json::parse(recs::testing::read_file(p)); }

void require_valid(const nlohmann::json& doc, const std::string& schema) {
  const auto errs = recs::testing::SchemaChecker::from_contract(schema).errors(doc);
  for (const auto& e : errs) UNSCOPED_INFO(e);
  CHECK(errs.empty());
}

}  // namespace

TEST_CASE("write_per_user writes the private per-user file", "[c1][per_user]") {
  Run run;
  const FixedScoreModel m({6.f, 5.f, 4.f, 3.f, 2.f, 1.f}, true);
  recs::EvalOptions opt;
  opt.keep_per_user = true;
  const auto r = recs::evaluate(m, run.train, run.heldout, run.users, opt);
  TempDir dir("per-user");
  const auto priv = dir.path() / "data" / "reports-private";
  const auto path = recs::write_per_user(run.header("2026-11-10T00-00-00Z_fixed", "fixed"), r, opt, priv);
  CHECK(path == priv / "2026-11-10T00-00-00Z_fixed.per_user.json");
  const auto doc = load(path);
  require_valid(doc, "per_user.schema.json");
  CHECK(doc["users"] == nlohmann::json::array({0, 1, 2}));
  CHECK(doc["n_relevant"] == nlohmann::json::array({1, 0, 1}));
  CHECK(doc["split_hash"] == run.split.hash);
  CHECK(doc["metrics"]["ndcg@10"][0].get<double>() == Approx(0.5));
  CHECK(doc["metrics"]["ndcg@10"][1].is_null());  // no relevant item
  CHECK(doc["metrics"]["map@10"][1].is_null());
  CHECK(doc["metrics"]["pop_rank@10"][1].get<double>() == Approx(3.8));  // every user has a pop rank
  CHECK(doc["rating"]["count"] == nlohmann::json::array({2, 1, 1}));

  // Never inside a served reports directory.
  CHECK_THROWS_AS(recs::write_per_user(run.header("x_fixed", "fixed"), r, opt, dir.path() / "data" / "reports"),
                  std::invalid_argument);
  CHECK_THROWS_AS(recs::write_per_user(run.header("x_fixed", "fixed"), r, opt, dir.path() / "data" / "reports" / "private"),
                  std::invalid_argument);
}

TEST_CASE("compare_runs: a run against itself has zero deltas", "[c1][compare]") {
  Run run;
  const FixedScoreModel m({6.f, 5.f, 4.f, 3.f, 2.f, 1.f}, true);
  recs::EvalOptions opt;
  const auto r = recs::evaluate(m, run.train, run.heldout, run.users, opt);
  TempDir dir("cmp-self");
  const auto a = load(recs::write_per_user(run.header("2026-11-10T00-00-00Z_fixed", "fixed"), r, opt, dir.path()));
  const auto c = recs::compare_runs(a, a, 200, 7);
  require_valid(c, "comparison.schema.json");
  CHECK(c["resamples"] == 200);
  CHECK(c["seed"] == 7);
  CHECK(c["a"]["run_id"] == "2026-11-10T00-00-00Z_fixed");
  CHECK(c["b"]["model"] == "fixed");
  CHECK(c["metrics"].contains("ndcg@10"));
  CHECK(c["metrics"].contains("rmse"));  // both runs predict ratings
  CHECK(c["metrics"]["ndcg@10"]["users"] == 2);
  CHECK(c["metrics"]["pop_rank@10"]["users"] == 3);
  for (const auto& [name, v] : c["metrics"].items()) {
    INFO(name);
    CHECK(v["mean_delta"].get<double>() == 0.0);
    CHECK(v["ci95"][0].get<double>() == 0.0);
    CHECK(v["ci95"][1].get<double>() == 0.0);
    CHECK(v["excludes_zero"] == false);
  }
}

TEST_CASE("compare_runs: deltas are a - b over common users", "[c1][compare]") {
  Run run;
  recs::EvalOptions opt;
  // a sinks item 1, so u0's relevant item 3 moves from rank 3 to rank 2 (NDCG 0.5 -> 1 / log2(3));
  // u2 (who trained on item 1) is unchanged. b is the fixture order.
  const FixedScoreModel ma({6.f, 0.f, 4.f, 3.f, 2.f, 1.f});
  const FixedScoreModel mb({6.f, 5.f, 4.f, 3.f, 2.f, 1.f});
  TempDir dir("cmp-ab");
  const auto a = load(recs::write_per_user(run.header("run_a", "a"), recs::evaluate(ma, run.train, run.heldout, run.users, opt), opt, dir.path()));
  const auto b = load(recs::write_per_user(run.header("run_b", "b"), recs::evaluate(mb, run.train, run.heldout, run.users, opt), opt, dir.path()));
  const auto c = recs::compare_runs(a, b, 100, 42);
  require_valid(c, "comparison.schema.json");
  CHECK(c["metrics"]["ndcg@10"]["mean_delta"].get<double>() == Approx((1.0 / std::log2(3.0) - 0.5) / 2.0));
  CHECK_FALSE(c["metrics"].contains("rmse"));  // neither predicts ratings

  auto other = b;
  other["split_hash"] = std::string(64, 'd');
  CHECK_THROWS_AS(recs::compare_runs(a, other, 100, 42), std::invalid_argument);
  other = b;
  other["evaluated_on"] = "validation";
  CHECK_THROWS_AS(recs::compare_runs(a, other, 100, 42), std::invalid_argument);
  other = b;
  other["schema_version"] = 2;
  CHECK_THROWS_AS(recs::compare_runs(a, other, 100, 42), std::invalid_argument);
}

TEST_CASE("make_run_id, write_report and peak_rss_bytes", "[c1][report]") {
  const auto id = recs::make_run_id("pop");
  CHECK(std::regex_match(id, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_pop)")));
  CHECK(std::regex_match(recs::make_run_id("als", "k64"), std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_als_k64)")));
  CHECK_THROWS_AS(recs::make_run_id("../x"), std::invalid_argument);
  CHECK_THROWS_AS(recs::make_run_id("pop", "a b"), std::invalid_argument);

  TempDir dir("write-report");
  nlohmann::json rep = {{"run_id", "2026-11-10T00-00-00Z_pop"}, {"b", 1}, {"a", {{"z", 1}, {"y", 2}}}};
  const auto path = recs::write_report(rep, dir.path() / "reports");
  CHECK(path == dir.path() / "reports" / "2026-11-10T00-00-00Z_pop.json");
  CHECK(recs::testing::read_file(path) == rep.dump(2) + "\n");
  CHECK(recs::testing::read_file(path).find("\"a\"") < recs::testing::read_file(path).find("\"b\""));
  CHECK_THROWS(recs::write_report(rep, dir.path() / "reports"));  // never overwrites a report
  CHECK_THROWS_AS(recs::write_report({{"run_id", "../escape"}}, dir.path()), std::invalid_argument);

  const auto rss = recs::peak_rss_bytes();
  REQUIRE(rss.has_value());
  CHECK(*rss > 0);
}
