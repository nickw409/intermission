// Regression tests for inputs found by the Model::load fuzzers (tests/fuzz/core/model_load_*.cpp).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../data/temp_dir.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace fs = std::filesystem;
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

  // The original file still loads, and fold-in + scoring stays NaN-free.
  dir.write("item_factors.bin", good);
  auto m = recs::make_model("mf");
  REQUIRE_NOTHROW(m->load(dir.path()));
  std::vector<float> out(m->n_items());
  m->score(m->fold_in(std::vector<recs::Rating>{{0, 4.5f, 0}, {3, 2.0f, 0}}), out);
  for (const float v : out) CHECK_FALSE(std::isnan(v));
}
