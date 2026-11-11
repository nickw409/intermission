// MF-SGD on real MovieLens data with the shipped config (configs/mf.json). Skipped when the data is
// not present (CI has none). The spec's Phase 3 gate is fold-in parity Jaccard@10 > 0.9 against
// trained user vectors; the ml-32m run is a release-build report (docs/phase-3.md). This test is
// the early warning on ml-latest-small: early stopping must not leave the trained user vectors so
// far from convergence that fold-in (the exact ridge solution) disagrees with them.
//
// It runs the metric exactly as `recs-train eval` reports it (recs::evaluate, 500 sampled users,
// seed 42) on both paths the CLI uses:
//   - tuning: fit on train_minus_val, early-stopped on the validation set;
//   - final:  fit on train (train + validation) for max_epochs, no validation.
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/eval.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"

namespace fs = std::filesystem;

namespace {

fs::path data_root() {
  if (const char* env = std::getenv("RECS_DATA_DIR"); env != nullptr && *env != '\0') return fs::path(env);
  return recs::testing::data_dir();
}

// configs/mf.json from the source tree (RECS_DATA_DIR is <source>/data).
recs::Config shipped_config() {
  const fs::path path = recs::testing::data_dir().parent_path() / "configs" / "mf.json";
  std::ifstream in(path);
  REQUIRE(in.good());
  return recs::Config::parse(in);
}

double parity(const recs::Model& m, const recs::Dataset& train, const recs::Split& split,
              const std::vector<std::vector<recs::Rating>>& heldout) {
  recs::EvalOptions opt;
  opt.bootstrap_resamples = 1;  // only the parity is used here
  const auto res = recs::evaluate(m, train, heldout, split.eval_users, opt);
  REQUIRE(res.foldin_jaccard10.has_value());
  return *res.foldin_jaccard10;
}

}  // namespace

TEST_CASE("MF on ml-latest-small with configs/mf.json: fold-in parity Jaccard@10 > 0.9",
          "[mf][ml-small][gate]") {
  const auto raw = data_root() / "raw" / "ml-latest-small";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-latest-small not present at " << raw);
  const auto split = recs::make_split(recs::load_movielens(raw, "ml-latest-small"), recs::SplitConfig{});
  const auto cfg = shipped_config();

  SECTION("tuning path: early-stopped on validation") {
    auto m = recs::make_model("mf");
    recs::TrainContext ctx;
    ctx.seed = 42;
    ctx.validation = &split.validation;
    m->fit(split.train_minus_val, cfg, ctx);
    const double j = parity(*m, split.train_minus_val, split, split.validation);
    INFO("epochs " << m->training_curve().size() << ", mean Jaccard@10 " << j);
    CHECK(j > 0.9);
  }
  SECTION("final path: train + validation, max_epochs, no validation") {
    auto m = recs::make_model("mf");
    recs::TrainContext ctx;
    ctx.seed = 42;
    m->fit(split.train, cfg, ctx);
    const double j = parity(*m, split.train, split, split.test);
    INFO("epochs " << m->training_curve().size() << ", mean Jaccard@10 " << j);
    CHECK(j > 0.9);
  }
}
