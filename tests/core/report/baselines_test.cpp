// Gate 5 (C1): rating-task baselines, hand-computed, and how make_report includes them.
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fake_models.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "schema_check.hpp"

using Catch::Approx;

namespace {

// Training ratings (mu = 21 / 6 = 3.5, damping lambda = 10):
//   u0: i0 = 5, i1 = 3     u1: i0 = 4, i2 = 2     u2: i0 = 3, i1 = 4
// Item biases b_i = sum(r - mu) / (n_i + 10):
//   b0 = (1.5 + 0.5 - 0.5) / 13 = 1.5 / 13,  b1 = 0 / 12 = 0,  b2 = -1.5 / 11,  b3 = 0 (unrated)
// User biases b_u = sum(r - mu) / (n_u + 10):
//   u0 = 1 / 12,  u1 = -1 / 12,  u2 = 0
// User bias on top of item biases (as in MF fold-in), b'_u = sum(r - mu - b_i) / (n_u + 10):
//   u0 = ((1.5 - b0) + (-0.5 - b1)) / 12,  u1 = ((0.5 - b0) + (-1.5 - b2)) / 12,
//   u2 = ((-0.5 - b0) + (0.5 - b1)) / 12
// Held out: u0: i3 = 4.0,  u1: i1 = 5.0,  u2: i2 = 1.0,  u3: nothing.
const double mu = 3.5;
const double b0 = 1.5 / 13.0, b1 = 0.0, b2 = -1.5 / 11.0, b3 = 0.0;
const double bu0 = 1.0 / 12.0, bu1 = -1.0 / 12.0, bu2 = 0.0;
const double cu0 = ((1.5 - b0) + (-0.5 - b1)) / 12.0;
const double cu1 = ((0.5 - b0) + (-1.5 - b2)) / 12.0;
const double cu2 = ((-0.5 - b0) + (0.5 - b1)) / 12.0;

recs::Dataset train() {
  return recs::make_dataset("bl", 4, 4,
                            {{0, 0, 5.f, 1}, {0, 1, 3.f, 2}, {1, 0, 4.f, 3},
                             {1, 2, 2.f, 4}, {2, 0, 3.f, 5}, {2, 1, 4.f, 6}});
}
const std::vector<std::vector<recs::Rating>> heldout = {{{3, 4.f, 9}}, {{1, 5.f, 9}}, {{2, 1.f, 9}}, {}};
const std::vector<recs::UserIdx> users = {0, 1, 2, 3};

void check_errors(const recs::RatingBaselineResult& r, std::vector<double> err) {
  INFO(r.name);
  REQUIRE(r.sq_err.size() == 4);
  REQUIRE(r.abs_err.size() == 4);
  REQUIRE(r.count.size() == 4);
  for (std::size_t u = 0; u < 3; ++u) {
    INFO("user " << u);
    CHECK(r.sq_err[u] == Approx(err[u] * err[u]).epsilon(1e-6));
    CHECK(r.abs_err[u] == Approx(std::abs(err[u])).epsilon(1e-6));
    CHECK(r.count[u] == 1.0);
  }
  CHECK(r.sq_err[3] == 0.0);
  CHECK(r.count[3] == 0.0);
}

}  // namespace

TEST_CASE("rating baselines match hand-computed values", "[c1][baselines]") {
  const auto res = recs::evaluate_rating_baselines(train(), heldout, users);
  REQUIRE(res.size() == 4);
  CHECK(res[0].name == "global_mean");
  CHECK(res[1].name == "user_bias");
  CHECK(res[2].name == "item_bias");
  CHECK(res[3].name == "user_item_bias");
  // err = actual - prediction
  check_errors(res[0], {4.0 - mu, 5.0 - mu, 1.0 - mu});
  check_errors(res[1], {4.0 - (mu + bu0), 5.0 - (mu + bu1), 1.0 - (mu + bu2)});
  check_errors(res[2], {4.0 - (mu + b3), 5.0 - (mu + b1), 1.0 - (mu + b2)});
  check_errors(res[3], {4.0 - (mu + b3 + cu0), 5.0 - (mu + b1 + cu1), 1.0 - (mu + b2 + cu2)});
}

TEST_CASE("rating baselines only score the requested users, in order", "[c1][baselines]") {
  const std::vector<recs::UserIdx> some = {2, 0};
  const auto res = recs::evaluate_rating_baselines(train(), heldout, some);
  REQUIRE(res[0].sq_err.size() == 2);
  CHECK(res[0].sq_err[0] == Approx(2.5 * 2.5));
  CHECK(res[0].sq_err[1] == Approx(0.25));
}

TEST_CASE("rating baselines sum several held-out ratings per user", "[c1][baselines]") {
  // u0 holds out i3 = 4.0 and i2 = 3.0; u2 holds out i3 = 2.0, i1 = 5.0 and i2 = 4.5.
  const std::vector<std::vector<recs::Rating>> ho = {{{3, 4.f, 9}, {2, 3.f, 9}}, {}, {{3, 2.f, 9}, {1, 5.f, 9}, {2, 4.5f, 9}}, {}};
  const std::vector<recs::UserIdx> us = {0, 2};
  const auto res = recs::evaluate_rating_baselines(train(), ho, us);
  // Per baseline: predictions for (u0: i3, i2) and (u2: i3, i1, i2).
  const std::vector<std::vector<std::vector<double>>> preds = {
      {{mu, mu}, {mu, mu, mu}},
      {{mu + bu0, mu + bu0}, {mu + bu2, mu + bu2, mu + bu2}},
      {{mu + b3, mu + b2}, {mu + b3, mu + b1, mu + b2}},
      {{mu + b3 + cu0, mu + b2 + cu0}, {mu + b3 + cu2, mu + b1 + cu2, mu + b2 + cu2}},
  };
  const std::vector<std::vector<double>> actual = {{4.0, 3.0}, {2.0, 5.0, 4.5}};
  REQUIRE(res.size() == 4);
  for (std::size_t b = 0; b < 4; ++b) {
    INFO(res[b].name);
    REQUIRE(res[b].sq_err.size() == 2);
    for (std::size_t j = 0; j < 2; ++j) {
      double sq = 0.0, ab = 0.0;
      for (std::size_t q = 0; q < actual[j].size(); ++q) {
        const double e = actual[j][q] - preds[b][j][q];
        sq += e * e;
        ab += std::abs(e);
      }
      CHECK(res[b].sq_err[j] == Approx(sq).epsilon(1e-9));
      CHECK(res[b].abs_err[j] == Approx(ab).epsilon(1e-9));
      CHECK(res[b].count[j] == static_cast<double>(actual[j].size()));
    }
  }
  // Global mean, written out: u0 = 0.5^2 + 0.5^2 = 0.5; u2 = 1.5^2 + 1.5^2 + 1^2 = 5.5.
  CHECK(res[0].sq_err[0] == Approx(0.5));
  CHECK(res[0].sq_err[1] == Approx(5.5));
  CHECK(res[0].abs_err[1] == Approx(4.0));
}

TEST_CASE("make_report fills rating_baselines when given", "[c1][baselines][report]") {
  const auto ds = train();
  recs::testing::FixedScoreModel m({4.f, 3.f, 2.f, 1.f});
  const auto r = recs::evaluate(m, ds, heldout, users, recs::EvalOptions{});
  const auto bl = recs::evaluate_rating_baselines(ds, heldout, users);

  recs::Split split;
  split.hash = std::string(64, 'b');
  split.train = ds;
  split.train_minus_val = ds;
  recs::ReportHeader h;
  h.run_id = "2026-11-10T00-00-00Z_fixed";
  h.split = &split;
  h.evaluated_on = "test";
  h.model = "fixed";
  h.config = recs::Config::object();
  h.seeds = {{"split", 42}};

  auto without = recs::make_report(h, r, recs::EvalOptions{});
  CHECK(without["rating_baselines"].is_null());
  CHECK(without["peak_rss_bytes"].is_null());

  h.rating_baselines = &bl;
  const auto rep = recs::make_report(h, r, recs::EvalOptions{});
  const auto errs = recs::testing::SchemaChecker::from_contract("report.schema.json").errors(rep);
  for (const auto& e : errs) UNSCOPED_INFO(e);
  CHECK(errs.empty());
  const auto& g = rep["rating_baselines"]["global_mean"];
  CHECK(g["n"] == 3);
  CHECK(g["rmse"]["mean"].get<double>() == Approx(std::sqrt((0.25 + 2.25 + 6.25) / 3.0)));
  CHECK(g["mae"]["mean"].get<double>() == Approx((0.5 + 1.5 + 2.5) / 3.0));
  CHECK(rep["rating_baselines"]["user_item_bias"]["n"] == 3);
}
