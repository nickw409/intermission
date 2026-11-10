// Gates 4 and 7 (C1): evaluate() metric scope, fold-in parity, the rating task, and make_report.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <omp.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fake_models.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "recs/rng.hpp"
#include "schema_check.hpp"
#include "test_util.hpp"

using Catch::Approx;
using recs::testing::FixedScoreModel;

namespace {

// 3 users x 6 items. Every user gets the same scores: item i scores 6 - i, so the global order is
// 0, 1, 2, 3, 4, 5.
//   u0 trains on {0};    held out: item 3 = 4.5 (relevant, gain 1.0), item 1 = 2.0
//   u1 trains on {1};    held out: item 2 = 3.0 (no relevant item)
//   u2 trains on {0, 1}; held out: item 2 = 4.0 (relevant, gain 0.5)
// Lists (training items excluded): u0 [1 2 3 4 5], u1 [0 2 3 4 5], u2 [2 3 4 5].
// Popularity ranks from training counts {2, 2, 0, 0, 0, 0}: item i has rank i + 1.
struct Fixture {
  recs::Dataset train = recs::make_dataset("eval-fixture", 3, 6,
                                           {{0, 0, 4.f, 1}, {1, 1, 3.f, 1}, {2, 0, 5.f, 1}, {2, 1, 2.f, 1}});
  std::vector<std::vector<recs::Rating>> heldout = {
      {{3, 4.5f, 9}, {1, 2.0f, 9}},
      {{2, 3.0f, 9}},
      {{2, 4.0f, 9}},
  };
  std::vector<recs::UserIdx> users = {0, 1, 2};
  FixedScoreModel model{{6.f, 5.f, 4.f, 3.f, 2.f, 1.f}};
};

recs::Split make_split_for(const recs::Dataset& train) {
  recs::Split s;
  s.hash = std::string(64, 'a');
  s.train = train;
  s.train_minus_val = train;
  s.cold_item_share = 0.25;
  return s;
}

recs::ReportHeader header_for(const recs::Split& split) {
  recs::ReportHeader h;
  h.run_id = "2026-11-10T00-00-00Z_fixed";
  h.git_sha = "abc1234";
  h.dataset = "eval-fixture";
  h.split = &split;
  h.evaluated_on = "test";
  h.model = "fixed";
  h.config = recs::Config{{"fixed", true}};
  h.seeds = {{"split", 42}, {"model", 42}, {"bootstrap", 42}, {"parity", 42}};
  h.train_seconds = 0.5;
  h.peak_rss_bytes = 1234;
  return h;
}

void require_valid(const nlohmann::json& doc, const std::string& schema) {
  const auto errs = recs::testing::SchemaChecker::from_contract(schema).errors(doc);
  for (const auto& e : errs) UNSCOPED_INFO(e);
  CHECK(errs.empty());
}

}  // namespace

TEST_CASE("schema checker accepts the report fixtures and rejects unknown_major", "[c1][report][schema]") {
  const auto checker = recs::testing::SchemaChecker::from_contract("report.schema.json");
  for (const char* f : {"pop.json", "pop_count.json", "als.json", "mf.json", "knn.json", "content.json"}) {
    INFO(f);
    const auto doc = nlohmann::json::parse(recs::testing::read_file(recs::testing::contracts_dir() / "fixtures" / "reports" / f));
    CHECK(checker.errors(doc).empty());
  }
  const auto bad = nlohmann::json::parse(
      recs::testing::read_file(recs::testing::contracts_dir() / "fixtures" / "reports" / "unknown_major.json"));
  CHECK_FALSE(checker.errors(bad).empty());
}

TEST_CASE("evaluate: per-user results and metric scope", "[c1][eval]") {
  Fixture f;
  recs::EvalOptions opt;
  opt.cold_train_ratings_lt = 2;
  const auto r = recs::evaluate(f.model, f.train, f.heldout, f.users, opt);

  REQUIRE(r.per_user.size() == 3);
  CHECK(r.users_without_relevant == 1);
  CHECK_FALSE(r.has_rating_task);
  CHECK_FALSE(r.foldin_jaccard10.has_value());  // the model has no batch path
  CHECK(r.eval_seconds >= 0.0);

  const auto& u0 = r.per_user[0];
  const auto& u1 = r.per_user[1];
  const auto& u2 = r.per_user[2];
  CHECK(u0.user == 0);
  CHECK(u1.user == 1);
  CHECK(u2.user == 2);
  CHECK(u0.n_train == 1);
  CHECK(u2.n_train == 2);
  CHECK(u0.n_relevant == 1);
  CHECK(u1.n_relevant == 0);
  CHECK(u2.n_relevant == 1);

  // ks = {1, 5, 10, 20, 50}. u0 hits at rank 3: DCG = 1 / log2(4) = 0.5, IDCG = 1.
  REQUIRE(u0.ndcg_at.size() == 5);
  CHECK(u0.ndcg_at[0] == Approx(0.0));
  CHECK(u0.ndcg_at[2] == Approx(0.5));
  CHECK(u0.recall_at[2] == Approx(1.0));
  CHECK(u0.precision_at[2] == Approx(0.1));
  CHECK(u0.map10 == Approx(1.0 / 3.0));
  CHECK(u2.ndcg_at[0] == Approx(1.0));
  CHECK(u2.map10 == Approx(1.0));
  // Users without a relevant item have empty ranking vectors.
  CHECK(u1.ndcg_at.empty());
  CHECK(u1.recall_at.empty());
  CHECK(u1.precision_at.empty());

  // Popularity rank is computed for every user: ranks of the top-10 list, averaged.
  CHECK(u0.mean_pop_rank10 == Approx((2 + 3 + 4 + 5 + 6) / 5.0));
  CHECK(u1.mean_pop_rank10 == Approx((1 + 3 + 4 + 5 + 6) / 5.0));
  CHECK(u2.mean_pop_rank10 == Approx((3 + 4 + 5 + 6) / 4.0));
  // Coverage over every evaluated user's top-10 (u1 contributes item 0): 6 / 6.
  CHECK(r.coverage10 == Approx(1.0));
}

TEST_CASE("make_report: means over relevant users; pop rank and coverage over all", "[c1][report]") {
  Fixture f;
  recs::EvalOptions opt;
  opt.cold_train_ratings_lt = 2;
  const auto r = recs::evaluate(f.model, f.train, f.heldout, f.users, opt);
  const auto split = make_split_for(f.train);
  const auto h = header_for(split);
  const auto rep = recs::make_report(h, r, opt);
  require_valid(rep, "report.schema.json");

  CHECK(rep["schema_version"] == 1);
  CHECK(rep["run_id"] == h.run_id);
  CHECK(rep["split_hash"] == split.hash);
  CHECK(rep["cold_item_share"] == 0.25);
  CHECK(rep["counts"]["users_evaluated"] == 3);
  CHECK(rep["counts"]["users_without_relevant"] == 1);
  CHECK(rep["counts"]["n_users"] == 3);
  CHECK(rep["counts"]["n_items"] == 6);
  CHECK(rep["counts"]["n_train_ratings"] == 4);
  CHECK(rep["split"]["min_ratings"] == 10);
  CHECK(rep["split"]["seed"] == 42);
  CHECK(rep.at("split").value("val_min", -1) == 1);
  CHECK(rep.at("split").value("val_max", -1) == 10);
  CHECK(rep["seeds"]["bootstrap"] == 42);
  CHECK(rep["peak_rss_bytes"] == 1234);
  CHECK(rep["train_seconds"] == 0.5);
  CHECK(rep["config"] == h.config);

  const auto& m = rep["metrics"];
  CHECK(m["ndcg@10"]["mean"].get<double>() == Approx(0.75));  // (0.5 + 1.0) / 2, u1 excluded
  CHECK(m["recall@10"]["mean"].get<double>() == Approx(1.0));
  CHECK(m["recall@50"]["mean"].get<double>() == Approx(1.0));
  CHECK(m["precision@10"]["mean"].get<double>() == Approx(0.1));
  CHECK(m["map@10"]["mean"].get<double>() == Approx((1.0 / 3.0 + 1.0) / 2.0));
  CHECK(m["mean_pop_rank@10"]["mean"].get<double>() == Approx((4.0 + 3.8 + 4.5) / 3.0));  // all users
  for (const auto& [name, est] : m.items()) {
    INFO(name);
    CHECK(est["ci95"][0].get<double>() <= est["mean"].get<double>() + 1e-12);
    CHECK(est["ci95"][1].get<double>() >= est["mean"].get<double>() - 1e-12);
  }
  CHECK(rep["aggregates"]["coverage@10"].get<double>() == Approx(1.0));

  // per_k curves are the plain means at each k, over users with a relevant item.
  REQUIRE(rep["per_k"]["ndcg"].size() == 5);
  CHECK(rep["per_k"]["ndcg"][0][0] == 1);
  CHECK(rep["per_k"]["ndcg"][0][1].get<double>() == Approx(0.5));
  CHECK(rep["per_k"]["ndcg"][2][0] == 10);
  CHECK(rep["per_k"]["ndcg"][2][1].get<double>() == Approx(0.75));
  CHECK(rep["per_k"]["precision"][4][0] == 50);
  CHECK(rep["per_k"]["precision"][4][1].get<double>() == Approx(1.0 / 50.0));
  CHECK(rep["per_k"]["recall"][0][1].get<double>() == Approx(0.5));

  // Cold slice: users with fewer than 2 training ratings are u0 and u1; only u0 has a relevant item.
  const auto& cold = rep["slices"]["cold_users"];
  CHECK(cold["train_ratings_lt"] == 2);
  CHECK(cold["users"] == 2);
  CHECK(cold["ndcg@10"]["mean"].get<double>() == Approx(0.5));

  CHECK(rep["rating_metrics"].is_null());
  CHECK(rep["rating_baselines"].is_null());
  CHECK(rep["foldin_jaccard@10"].is_null());
  CHECK(rep["training_curve"] == nlohmann::json::array());
  CHECK(rep["training_curve_metric"] == "");
}

TEST_CASE("make_report: cold slice without relevant users is null; options are validated", "[c1][report]") {
  Fixture f;
  recs::EvalOptions opt;
  opt.cold_train_ratings_lt = 1;  // nobody has fewer than 1 training rating
  const auto r = recs::evaluate(f.model, f.train, f.heldout, f.users, opt);
  const auto split = make_split_for(f.train);
  const auto rep = recs::make_report(header_for(split), r, opt);
  require_valid(rep, "report.schema.json");
  CHECK(rep["slices"]["cold_users"]["users"] == 0);
  CHECK(rep["slices"]["cold_users"]["ndcg@10"].is_null());

  recs::EvalOptions no50 = opt;
  no50.ks = {1, 5, 10};
  CHECK_THROWS_AS(recs::evaluate(f.model, f.train, f.heldout, f.users, no50), std::invalid_argument);
  recs::ReportHeader bad = header_for(split);
  bad.evaluated_on = "train";
  CHECK_THROWS_AS(recs::make_report(bad, r, opt), std::invalid_argument);
  bad = header_for(split);
  bad.split = nullptr;
  CHECK_THROWS_AS(recs::make_report(bad, r, opt), std::invalid_argument);
}

TEST_CASE("evaluate: fold-in parity runs only for models with a batch path", "[c1][eval][parity]") {
  Fixture f;
  recs::EvalOptions opt;
  const std::vector<float> s = {6.f, 5.f, 4.f, 3.f, 2.f, 1.f};

  const FixedScoreModel same(s, false, true);
  const auto r1 = recs::evaluate(same, f.train, f.heldout, f.users, opt);
  REQUIRE(r1.foldin_jaccard10.has_value());
  CHECK(*r1.foldin_jaccard10 == Approx(1.0));

  // The batch path returns nothing for u1: Jaccard 0 for that user, 1 for the others.
  const FixedScoreModel broken(s, false, true, {1});
  const auto r2 = recs::evaluate(broken, f.train, f.heldout, f.users, opt);
  REQUIRE(r2.foldin_jaccard10.has_value());
  CHECK(*r2.foldin_jaccard10 == Approx(2.0 / 3.0));

  // A sample smaller than the population is seeded and reproducible.
  opt.parity_users = 2;
  const auto a = recs::evaluate(broken, f.train, f.heldout, f.users, opt);
  const auto b = recs::evaluate(broken, f.train, f.heldout, f.users, opt);
  REQUIRE(a.foldin_jaccard10.has_value());
  CHECK(*a.foldin_jaccard10 == *b.foldin_jaccard10);
  CHECK((*a.foldin_jaccard10 == Approx(0.5) || *a.foldin_jaccard10 == Approx(1.0)));

  const auto rep = recs::make_report(header_for(make_split_for(f.train)), r2, recs::EvalOptions{});
  CHECK(rep["foldin_jaccard@10"].get<double>() == Approx(2.0 / 3.0));
}

TEST_CASE("evaluate: rating task for models that predict ratings", "[c1][eval][rating]") {
  Fixture f;
  const FixedScoreModel m({3.f, 3.f, 3.f, 3.f, 3.f, 3.f}, true);
  recs::EvalOptions opt;
  const auto r = recs::evaluate(m, f.train, f.heldout, f.users, opt);
  CHECK(r.has_rating_task);
  // u0: (4.5 - 3)^2 + (2 - 3)^2 = 3.25, |1.5| + |1| = 2.5; u1: 0; u2: 1, 1.
  CHECK(r.per_user[0].sq_err == Approx(3.25));
  CHECK(r.per_user[0].abs_err == Approx(2.5));
  CHECK(r.per_user[0].n_rated == 2);
  CHECK(r.per_user[1].sq_err == Approx(0.0));
  CHECK(r.per_user[1].n_rated == 1);
  CHECK(r.per_user[2].sq_err == Approx(1.0));

  const auto split = make_split_for(f.train);
  const auto rep = recs::make_report(header_for(split), r, opt);
  require_valid(rep, "report.schema.json");
  CHECK(rep["rating_metrics"]["n"] == 4);
  CHECK(rep["rating_metrics"]["rmse"]["mean"].get<double>() == Approx(std::sqrt(4.25 / 4.0)));
  CHECK(rep["rating_metrics"]["mae"]["mean"].get<double>() == Approx(3.5 / 4.0));
}

TEST_CASE("evaluate: a rating model that cannot score a held-out item predicts the global mean", "[c1][eval][rating]") {
  Fixture f;
  // Item 2 is "cold" for this model. Training ratings are 4, 3, 5, 2: global mean 3.5.
  const float inf = std::numeric_limits<float>::infinity();
  const FixedScoreModel m({3.f, 3.f, -inf, 3.f, 3.f, 3.f}, true);
  const auto r = recs::evaluate(m, f.train, f.heldout, f.users, recs::EvalOptions{});
  REQUIRE(r.has_rating_task);
  // u1: item 2 rated 3.0 -> error 0.5; u2: item 2 rated 4.0 -> error 0.5. Both still counted.
  CHECK(r.per_user[1].n_rated == 1);
  CHECK(r.per_user[1].sq_err == Approx(0.25));
  CHECK(r.per_user[2].abs_err == Approx(0.5));
  // u0's items are scorable: unchanged.
  CHECK(r.per_user[0].sq_err == Approx(3.25));
}

TEST_CASE("evaluate is identical with 1 and 4 threads", "[c1][eval][determinism]") {
  // A random-ish dataset large enough to split work across threads.
  std::vector<recs::Triplet> t;
  recs::Rng rng(7);
  const std::uint32_t n_users = 60, n_items = 40;
  std::vector<std::vector<recs::Rating>> heldout(n_users);
  for (std::uint32_t u = 0; u < n_users; ++u) {
    for (std::uint32_t i = 0; i < n_items; ++i) {
      const auto x = rng.uniform(10);
      const float v = static_cast<float>(1 + rng.uniform(10)) / 2.f;
      if (x < 3) t.push_back({u, i, v, static_cast<std::int64_t>(i)});
      else if (x == 3) heldout[u].push_back({i, v, 100});
    }
  }
  const auto train = recs::make_dataset("rand", n_users, n_items, t);
  auto pop = recs::make_popularity();
  pop->fit(train, recs::Config::object(), {});
  std::vector<recs::UserIdx> users(n_users);
  for (std::uint32_t u = 0; u < n_users; ++u) users[u] = u;
  recs::EvalOptions opt;
  const auto split = make_split_for(train);
  auto run = [&](int threads) {
    omp_set_num_threads(threads);
    const auto r = recs::evaluate(*pop, train, heldout, users, opt);
    auto rep = recs::make_report(header_for(split), r, opt);
    for (const char* k : {"run_id", "created_at", "train_seconds", "eval_seconds", "peak_rss_bytes"}) rep.erase(k);
    return rep.dump(2);
  };
  const auto one = run(1);
  const auto four = run(4);
  const auto again = run(4);
  omp_set_num_threads(2);
  CHECK(one == four);
  CHECK(four == again);
}
