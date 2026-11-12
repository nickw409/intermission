// Regression tests for model files that loaded but then produced NaN or +inf scores: inputs found
// by the Model::load fuzzers (tests/fuzz/core/model_load_*.cpp) and by the SA2 re-test.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "../../fuzz/core/score_check.hpp"
#include "../data/temp_dir.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using recs::testing::TempDir;

namespace {

std::string read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

recs::Dataset small_dataset() {
  return recs::make_dataset("mf-fuzz", 3, 4,
                            {{0, 0, 5.f, 1}, {0, 1, 3.f, 2}, {1, 0, 4.f, 3}, {1, 2, 2.f, 4}, {2, 0, 3.f, 5}, {2, 1, 4.f, 6}});
}

}  // namespace

// fuzz_model_load_mf crash: item_factors.bin with a finite but absurd global mean (-1.1e297)
// loaded fine, and fold_in + score then produced NaN scores (forbidden by the Model contract).
// Every stored parameter (mu, b_i, q_if) must now be finite and at most 1e6 in magnitude.
TEST_CASE("mf load rejects finite but absurd parameters", "[security][fuzz][mf]") {
  TempDir dir("mf-fuzz-mu");
  auto fitted = recs::make_model("mf");
  fitted->fit(small_dataset(), recs::Config{{"k", 3}, {"max_epochs", 3}}, {});
  fitted->save(dir.path());
  const std::string good = read_bytes(dir / "item_factors.bin");
  constexpr std::size_t kMuOffset = 8 + 4 + 4 + 4;  // magic, version, n_items, k
  constexpr std::size_t kBiOffset = kMuOffset + 8;

  for (const double mu : {-1.111961655425476e+297, 1e300, 1e9, -1e7}) {
    INFO("mu = " << mu);
    std::string bad = good;
    std::memcpy(bad.data() + kMuOffset, &mu, sizeof mu);
    dir.write("item_factors.bin", bad);
    auto m = recs::make_model("mf");
    CHECK_THROWS(m->load(dir.path()));
  }
  for (const float v : {3e38f, -1e7f}) {
    INFO("b_0 = " << v << ", last q = " << v);
    std::string bad_bi = good, bad_q = good;
    std::memcpy(bad_bi.data() + kBiOffset, &v, sizeof v);
    std::memcpy(bad_q.data() + bad_q.size() - sizeof v, &v, sizeof v);
    for (const auto& bad : {bad_bi, bad_q}) {
      dir.write("item_factors.bin", bad);
      auto m = recs::make_model("mf");
      CHECK_THROWS(m->load(dir.path()));
    }
  }

  // The original file still loads, and fold-in + scoring gives no NaN or +inf scores.
  dir.write("item_factors.bin", good);
  auto m = recs::make_model("mf");
  REQUIRE_NOTHROW(m->load(dir.path()));
  std::vector<float> out(m->n_items());
  m->score(m->fold_in(std::vector<recs::Rating>{{0, 4.5f, 0}, {3, 2.0f, 0}}), out);
  for (const float v : out) CHECK(recs::fuzz::score_allowed(v));
}

// SA2-7: a crafted ALS artifact (lambda = 1e-300, k = 1, Y = [1e-39, 0]) loaded, fold-in overflowed
// to infinity and score() returned NaN, so every als request failed. ALS now bounds lambda to
// [1e-6, 1e6], alpha to [0, 1e6] and every factor to |y| <= 1e6, at fit and at load.
TEST_CASE("SA2-7: als load rejects a degenerate lambda, alpha or factor", "[security][sa2][als]") {
  TempDir dir("sa2-7-als");
  const auto write_model = [&](double lambda, double alpha, float y0) {
    auto cfg = recs::make_model("als")->config();
    cfg["k"] = 1;
    cfg["lambda"] = lambda;
    cfg["alpha"] = alpha;
    dir.write("config.json", cfg.dump());
    std::string b("RECSALS1", 8);
    const auto u32 = [&](std::uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
    const auto f32 = [&](float v) { b.append(reinterpret_cast<const char*>(&v), 4); };
    u32(1);  // format version
    u32(2);  // n_items
    u32(1);  // k
    b.push_back(1);
    b.push_back(1);
    f32(y0);
    f32(0.0f);
    dir.write("factors.bin", b);
  };
  const auto load_error = [&]() -> std::string {
    auto m = recs::make_model("als");
    try {
      m->load(dir.path());
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  };

  write_model(1e-300, 40.0, 1e-39f);  // the audit's input
  CHECK_THAT(load_error(), ContainsSubstring("lambda"));
  write_model(1e-7, 40.0, 0.5f);
  CHECK_THAT(load_error(), ContainsSubstring("lambda"));
  write_model(2e6, 40.0, 0.5f);
  CHECK_THAT(load_error(), ContainsSubstring("lambda"));
  write_model(0.05, 2e6, 0.5f);
  CHECK_THAT(load_error(), ContainsSubstring("alpha"));
  write_model(0.05, 40.0, 2e6f);
  CHECK_THAT(load_error(), ContainsSubstring("out of range"));

  // At the limits (with a denormal factor): loads, and fold-in + score stay finite.
  write_model(1e-6, 1e6, 1e-39f);
  REQUIRE(load_error().empty());
  auto m = recs::make_model("als");
  m->load(dir.path());
  std::vector<float> out(2);
  m->score(m->fold_in(std::vector<recs::Rating>{{0, 5.0f, 0}}), out);
  for (const float v : out) CHECK(recs::fuzz::score_allowed(v));
}

// MF: lambda comes from config.json. With lambda = 0 the fold-in ridge falls to 1e-9 and the solve
// can overflow, so a servable model needs lambda in [1e-6, 1e6]: save() refuses to write one
// outside that range (training with lambda = 0 stays allowed) and load() refuses to read one.
TEST_CASE("mf save and load require lambda in [1e-6, 1e6]", "[security][mf]") {
  TempDir dir("mf-lambda");
  auto zero = recs::make_model("mf");
  zero->fit(small_dataset(), recs::Config{{"k", 3}, {"max_epochs", 3}, {"lambda", 0.0}}, {});
  CHECK_THROWS_WITH(zero->save(dir.path()), ContainsSubstring("lambda"));

  auto fitted = recs::make_model("mf");
  fitted->fit(small_dataset(), recs::Config{{"k", 3}, {"max_epochs", 3}}, {});
  fitted->save(dir.path());
  const auto cfg_text = read_bytes(dir / "config.json");
  for (const double lambda : {0.0, 1e-300, 1e-7, 2e6}) {
    INFO("lambda = " << lambda);
    auto cfg = nlohmann::json::parse(cfg_text);
    cfg["lambda"] = lambda;
    dir.write("config.json", cfg.dump());
    auto m = recs::make_model("mf");
    CHECK_THROWS_WITH(m->load(dir.path()), ContainsSubstring("lambda"));
  }
  dir.write("config.json", cfg_text);
  auto m = recs::make_model("mf");
  CHECK_NOTHROW(m->load(dir.path()));
}

// MF save() enforces the same parameter bound as load(), so an out-of-range model fails when it is
// written (at train time) rather than when recs-model loads it.
TEST_CASE("mf save rejects parameters that load would reject", "[security][mf]") {
  TempDir dir("mf-save-bound");
  auto m = recs::make_model("mf");
  // init_sd = 1e7 with a negligible learning rate leaves |q| far above 1e6 after one epoch.
  m->fit(small_dataset(), recs::Config{{"k", 3}, {"max_epochs", 1}, {"init_sd", 1e7}, {"lr", 1e-30}}, {});
  CHECK_THROWS_WITH(m->save(dir.path()), ContainsSubstring("out of range"));
}

// fuzz_model_load_pop_count crash (#80, once the fuzzers trapped +inf as well as NaN): a scores.bin
// holding +inf loaded, since the loader rejected only NaN. fit() never writes an infinite score
// (pop is a shrunk mean, pop_count a count), so both variants now reject any non-finite score.
TEST_CASE("pop load rejects non-finite scores", "[security][fuzz][popularity]") {
  for (const char* name : {"pop", "pop_count"}) {
    TempDir dir("pop-fuzz-inf");
    auto fitted = recs::make_model(name);
    fitted->fit(small_dataset(), recs::Config::object(), {});
    fitted->save(dir.path());
    const std::string good = read_bytes(dir / "scores.bin");

    for (const float v : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
      INFO(name << ": last score = " << v);
      std::string bad = good;
      std::memcpy(bad.data() + bad.size() - sizeof v, &v, sizeof v);  // scores are the file's tail
      dir.write("scores.bin", bad);
      auto m = recs::make_model(name);
      CHECK_THROWS_WITH(m->load(dir.path()), ContainsSubstring("non-finite score"));
    }

    dir.write("scores.bin", good);
    auto m = recs::make_model(name);
    REQUIRE_NOTHROW(m->load(dir.path()));
  }
}
