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
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "../../fuzz/core/model_check.hpp"
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
// holding +inf loaded, since the loader rejected only NaN. save() never writes a non-finite score
// (see the next test), so both variants now reject one on load.
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

// The save side of the rule above (#80 review): pop accepts any finite m >= 0, and with m = 1e308
// the shrinkage term m * C overflows, so fit() produced +inf for every item. save() must refuse to
// write what load() would reject, as mf and als already do.
TEST_CASE("pop save rejects non-finite scores", "[security][fuzz][popularity]") {
  TempDir dir("pop-save-inf");
  auto m = recs::make_model("pop");
  m->fit(small_dataset(), recs::Config{{"m", 1e308}}, {});
  CHECK_THROWS_WITH(m->save(dir.path()), ContainsSubstring("non-finite score"));
}

namespace {

// A hand-written scores.bin (layout in core/src/models/popularity.cpp): variant 0 = pop,
// 1 = pop_count.
std::string pop_file(std::uint32_t variant, double m, double c, const std::vector<std::uint32_t>& counts,
                     const std::vector<float>& scores) {
  std::string b("RECSPOP1", 8);
  const auto put = [&](const auto v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); };
  put(std::uint32_t{1});  // format version
  put(variant);
  put(m);
  put(c);
  put(static_cast<std::uint32_t>(counts.size()));
  for (const std::uint32_t v : counts) put(v);
  for (const float v : scores) put(v);
  return b;
}

std::string pop_load_error(const char* name, const TempDir& dir) {
  auto m = recs::make_model(name);
  try {
    m->load(dir.path());
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

}  // namespace

// #95: the popularity loader cross-checks scores.bin the way knn and content check theirs. fit()
// stores m = 0 for pop_count, a global mean C inside the rating range [0.5, 5] (or exactly 0 when
// there were no ratings), pop_count scores equal to the stored counts, and pop scores that are
// weighted means of ratings and C, so inside the rating range too. A file breaking any of these
// was not written by fit(), so load() rejects it.
TEST_CASE("pop load rejects inconsistent scores.bin", "[security][fuzz][popularity]") {
  TempDir dir("pop-cross-check");
  const auto rejects = [&](const char* name, const std::string& file, const char* what) {
    dir.write("scores.bin", file);
    CHECK_THAT(pop_load_error(name, dir), ContainsSubstring(what));
  };
  const auto loads = [&](const char* name, const std::string& file) {
    dir.write("scores.bin", file);
    CHECK(pop_load_error(name, dir).empty());
  };

  // pop_count: m must be 0.
  rejects("pop_count", pop_file(1, 10.0, 3.5, {2, 1}, {2.0f, 1.0f}), "m must be 0");
  rejects("pop_count", pop_file(1, 1e-300, 3.5, {2, 1}, {2.0f, 1.0f}), "m must be 0");
  // pop_count: every score equals its count.
  rejects("pop_count", pop_file(1, 0.0, 3.5, {2, 1}, {2.0f, 7.0f}), "score 1 differs from its count");
  rejects("pop_count", pop_file(1, 0.0, 3.5, {2, 1}, {2.0f, 1.5f}), "score 1 differs from its count");
  rejects("pop_count", pop_file(1, 0.0, 3.5, {2, 1}, {2.0f, -1.0f}), "score 1 differs from its count");

  for (const auto& [name, variant, m] : {std::tuple{"pop", 0u, 10.0}, std::tuple{"pop_count", 1u, 0.0}}) {
    INFO(name);
    const std::vector<std::uint32_t> counts{2, 1, 0};
    const std::vector<float> scores = variant == 1 ? std::vector<float>{2.0f, 1.0f, 0.0f}
                                                   : std::vector<float>{4.0f, 3.5f, 3.5f};
    // The global mean must lie in the rating range when there are ratings ...
    for (const double c : {0.0, 0.49, -1.0, 5.01, 1e300, -1e300}) {
      INFO("C = " << c);
      rejects(name, pop_file(variant, m, c, counts, scores), "global mean is outside the rating range");
    }
    // ... and be exactly 0 when there are none (fit() on an empty training set).
    rejects(name, pop_file(variant, m, 3.5, {0, 0}, {0.0f, 0.0f}), "global mean is nonzero with no ratings");
    // The edges of the range, and the empty case, load.
    loads(name, pop_file(variant, m, 0.5, counts, variant == 1 ? scores : std::vector<float>{0.5f, 0.5f, 0.5f}));
    loads(name, pop_file(variant, m, 5.0, counts, variant == 1 ? scores : std::vector<float>{5.0f, 5.0f, 5.0f}));
    loads(name, pop_file(variant, m, 0.0, {0, 0}, {0.0f, 0.0f}));
  }

  // pop: a score is a weighted mean of ratings and C, so it lies in the rating range (or is 0 when
  // there are no ratings at all).
  for (const float s : {0.0f, 0.49f, 5.01f, -3.0f, 1e30f, -1e30f}) {
    INFO("pop score = " << s);
    rejects("pop", pop_file(0, 10.0, 3.5, {2, 1}, {4.0f, s}), "score 1 is outside the rating range");
  }
  rejects("pop", pop_file(0, 10.0, 0.0, {0, 0}, {0.0f, 1.0f}), "score 1 is nonzero with no ratings");

  // Files written by fit() + save() still load, for both variants, including an empty training set,
  // and for pop with m = 0 (unrated items score exactly C) and with a very large finite m (every
  // score is then ~C).
  const auto round_trip = [&](const char* name, const recs::Dataset& ds, const recs::Config& cfg) {
    INFO(name << " " << cfg.dump() << " on " << ds.name);
    auto fitted = recs::make_model(name);
    fitted->fit(ds, cfg, {});
    fitted->save(dir.path());
    CHECK(pop_load_error(name, dir).empty());
  };
  for (const auto& ds : {small_dataset(), recs::make_dataset("pop-empty", 2, 3, {})}) {
    round_trip("pop", ds, recs::Config::object());
    round_trip("pop_count", ds, recs::Config::object());
    round_trip("pop", ds, recs::Config{{"m", 0.0}});
    round_trip("pop", ds, recs::Config{{"m", 1e300}});
  }
}

// PR #104 review: save() must refuse what load() would reject. Ratings outside [0.5, 5] put C (and
// pop scores) outside the rating range, so fit() + save() on them used to write a file the stricter
// load() refuses.
TEST_CASE("pop save rejects a file load would reject", "[security][popularity]") {
  TempDir dir("pop-save-consistent");
  const auto ds = recs::make_dataset("pop-out-of-range", 2, 2, {{0, 0, 9.0f, 1}, {1, 1, 8.0f, 2}});
  for (const char* name : {"pop", "pop_count"}) {
    INFO(name);
    auto m = recs::make_model(name);
    m->fit(ds, recs::Config::object(), {});
    CHECK_THROWS_AS(m->save(dir.path()), std::invalid_argument);
    CHECK_THROWS_WITH(m->save(dir.path()), ContainsSubstring("global mean is outside the rating range"));
  }
}

// pop_count ignores m, but fit() with a null config used to keep pop's default m = 10, which the
// stricter load() above would then refuse. fit() now stores m = 0 for pop_count whatever the config.
TEST_CASE("pop_count stores m = 0 even with a null config", "[security][popularity]") {
  TempDir dir("pop-count-null-cfg");
  auto fitted = recs::make_model("pop_count");
  fitted->fit(small_dataset(), recs::Config(), {});
  fitted->save(dir.path());
  CHECK(pop_load_error("pop_count", dir).empty());
}

// PR #104 review: content explain() could return a +inf weight from a model that loads. With
// user_mean centering and the ratings the fuzz harness uses (4.5 and 2.0, so w = +-1.25), item
// vectors v_0 = {f0: 1} and v_1 = {f0: 1, f1: 1.4e-45} cancel in f0, leaving a profile of norm
// ~1.4e-45. Each contribution c_j = w_j (v_j . v_0) / (|p| |v_0|) is then ~9e44, which overflows
// float; the +inf one passed the "weight > 0" filter. explain() now drops non-finite contributions.
TEST_CASE("content explain gives no +inf weight when the profile nearly cancels", "[security][fuzz][content]") {
  TempDir dir("content-explain-inf");
  constexpr float kDenormMin = std::numeric_limits<float>::denorm_min();  // 1.4e-45
  std::vector<recs::MovieMeta> movies(2);
  for (std::size_t i = 0; i < movies.size(); ++i) {
    movies[i].movie_id = static_cast<std::int64_t>(i + 1);
    movies[i].title = "Movie " + std::to_string(i + 1);
  }
  auto ds = recs::make_dataset("content-explain-inf", 2, 2,
                               {{0, 0, 4.0f, 1}, {0, 1, 3.0f, 2}, {1, 0, 2.0f, 3}, {1, 1, 5.0f, 4}}, movies);
  recs::GenomeMatrix g;
  g.n_items = 2;
  g.n_tags = 2;
  g.tag_names = {"f0", "f1"};
  g.relevance = {1.0f, 0.0f,          // v_0
                 1.0f, kDenormMin};   // v_1
  ds.genome = g;
  auto fitted = recs::make_model("content");
  fitted->fit(ds, recs::Config{{"centering", "user_mean"}}, {});
  fitted->save(dir.path());
  auto m = recs::make_model("content");
  REQUIRE_NOTHROW(m->load(dir.path()));

  const auto u = m->fold_in(std::vector<recs::Rating>{{0, 4.5f, 0}, {1, 2.0f, 0}});
  for (const recs::ItemIdx item : {0u, 1u}) {
    INFO("explain item " << item);
    for (const auto& b : m->explain(u, item).because) CHECK(std::isfinite(b.weight));
  }
  CHECK(recs::fuzz::check_loaded_model(*m).empty());
}
