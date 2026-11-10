// Content model (brief E gate 6): results do not depend on the thread count, save/load round-trips
// to bit-identical scores, and load() rejects damaged or hostile files.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <omp.h>

#include <catch2/catch_test_macros.hpp>

#include "content_fixtures.hpp"
#include "property.hpp"
#include "recs/model.hpp"

using namespace recs;
using namespace recs::testing::content;
namespace fs = std::filesystem;

namespace {

struct Outputs {
  std::vector<std::vector<float>> scores;
  std::vector<std::vector<Scored>> similar;
  std::vector<std::vector<Because>> explain;
};

Outputs outputs_of(const Model& m, const Dataset& ds) {
  Outputs o;
  for (UserIdx u = 0; u < ds.n_users(); ++u) {
    const auto state = m.fold_in(user_ratings(ds, u));
    o.scores.push_back(scores_of(m, state));
    for (ItemIdx i = 0; i < ds.n_items(); i += 7) o.explain.push_back(m.explain(state, i).because);
  }
  for (ItemIdx i = 0; i < ds.n_items(); ++i) o.similar.push_back(m.similar(i, 10));
  return o;
}

bool same(const Outputs& a, const Outputs& b) {
  if (a.scores.size() != b.scores.size()) return false;
  for (std::size_t k = 0; k < a.scores.size(); ++k)
    if (!bitwise_equal(a.scores[k], b.scores[k])) return false;
  if (a.similar != b.similar || a.explain.size() != b.explain.size()) return false;
  for (std::size_t k = 0; k < a.explain.size(); ++k) {
    if (a.explain[k].size() != b.explain[k].size()) return false;
    for (std::size_t j = 0; j < a.explain[k].size(); ++j)
      if (a.explain[k][j].item != b.explain[k][j].item ||
          std::bit_cast<std::uint32_t>(a.explain[k][j].weight) != std::bit_cast<std::uint32_t>(b.explain[k][j].weight))
        return false;
  }
  return true;
}

class ThreadCount {
 public:
  explicit ThreadCount(int n) : saved_(omp_get_max_threads()) { omp_set_num_threads(n); }
  ~ThreadCount() { omp_set_num_threads(saved_); }

 private:
  int saved_;
};

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& p, const std::string& bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("Content determinism: 1 thread and 4 threads give bit-identical results", "[content][determinism]") {
  recs::testing::for_seeds(3, 5150, [](Rng& rng, std::uint64_t) {
    const auto ds = random_dataset(rng, 30, 80, 0.2);
    Outputs one, four;
    {
      ThreadCount t(1);
      auto m = make_content();
      m->fit(ds, Config::object(), TrainContext{});
      one = outputs_of(*m, ds);
    }
    {
      ThreadCount t(4);
      auto m = make_content();
      m->fit(ds, Config::object(), TrainContext{});
      four = outputs_of(*m, ds);
    }
    CHECK(same(one, four));
  });
}

TEST_CASE("Content save/load: a loaded model scores bit-identically", "[content][persist]") {
  Rng rng(808);
  for (bool with_genome : {false, true}) {
    INFO("genome = " << with_genome);
    auto ds = random_dataset(rng, 20, 50, 0.3);
    if (with_genome) {
      GenomeMatrix g;
      g.n_items = ds.n_items();
      g.n_tags = 6;
      for (std::uint32_t k = 0; k < 6; ++k) g.tag_names.push_back("g" + std::to_string(k));
      for (std::uint32_t k = 0; k < g.n_items * g.n_tags; ++k)
        g.relevance.push_back(rng.uniform01() < 0.3 ? 0.0f : static_cast<float>(rng.uniform01()));
      ds.genome = g;
    }
    auto trained = make_content();
    trained->fit(ds, Config{{"min_tag_count", 2}, {"genre_weight", 0.5}}, TrainContext{});
    ScratchDir dir;
    trained->save(dir.path());

    auto loaded = make_content();
    loaded->load(dir.path());
    CHECK(loaded->n_items() == trained->n_items());
    CHECK(loaded->config() == trained->config());
    CHECK(loaded->supports_similar());
    CHECK(same(outputs_of(*trained, ds), outputs_of(*loaded, ds)));

    // Only files inside the directory are used: a copy elsewhere loads the same.
    ScratchDir other;
    for (const auto& e : fs::directory_iterator(dir.path())) fs::copy(e.path(), other.path() / e.path().filename());
    auto copied = make_content();
    copied->load(other.path());
    CHECK(same(outputs_of(*trained, ds), outputs_of(*copied, ds)));
  }
}

TEST_CASE("Content load: damaged or hostile files are rejected, never trusted", "[content][persist]") {
  Rng rng(4242);
  const auto ds = random_dataset(rng, 10, 30, 0.3);
  auto trained = make_content();
  trained->fit(ds, Config::object(), TrainContext{});
  ScratchDir dir;
  trained->save(dir.path());

  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir.path())) files.push_back(e.path());
  REQUIRE(!files.empty());

  fs::path bin;
  for (const auto& f : files)
    if (f.extension() == ".bin") bin = f;
  REQUIRE(!bin.empty());

  // Truncation at many lengths, trailing garbage, and a missing file, for every file.
  for (const auto& file : files) {
    INFO("file " << file.filename());
    const std::string good = read_file(file);
    REQUIRE(!good.empty());
    for (std::size_t n = 0; n < good.size(); n += std::max<std::size_t>(1, good.size() / 97)) {
      INFO("truncated to " << n);
      write_file(file, good.substr(0, n));
      auto m = make_content();
      CHECK_THROWS(m->load(dir.path()));
    }
    write_file(file, good + std::string("xyz"));
    {
      auto m = make_content();
      CHECK_THROWS(m->load(dir.path()));
    }
    fs::remove(file);
    {
      auto m = make_content();
      CHECK_THROWS(m->load(dir.path()));
    }
    write_file(file, good);
  }

  // Single-byte corruption anywhere in the binary file. Flipping bits in a float value can give
  // another valid model, so not every flip must throw; what must hold is that load() either throws
  // or yields a usable model (no crash, no NaN scores, sanitizer-clean).
  const std::string good = read_file(bin);
  std::size_t rejected = 0, positions = 0;
  for (std::size_t pos = 0; pos < good.size(); pos += std::max<std::size_t>(1, good.size() / 211)) {
    ++positions;
    std::string bad = good;
    bad[pos] = static_cast<char>(bad[pos] ^ 0x5A);
    write_file(bin, bad);
    auto m = make_content();
    try {
      m->load(dir.path());
    } catch (const std::exception&) {
      ++rejected;
      continue;
    }
    const auto state = m->fold_in(user_ratings(ds, 0));
    for (float s : scores_of(*m, state)) CHECK(!std::isnan(s));
  }
  INFO("rejected " << rejected << " of " << positions);
  CHECK(rejected > 0);

  // Huge counts in the header are rejected before anything is allocated.
  std::string huge = good;
  for (std::size_t k = 8; k < std::min<std::size_t>(64, huge.size()); ++k) huge[k] = static_cast<char>(0xFF);
  write_file(bin, huge);
  {
    auto m = make_content();
    CHECK_THROWS(m->load(dir.path()));
  }
  write_file(bin, good);
  auto ok = make_content();
  CHECK_NOTHROW(ok->load(dir.path()));
}

TEST_CASE("Content: score_trained_user is unavailable after load (no training matrix is saved)",
          "[content][persist]") {
  Rng rng(1);
  const auto ds = random_dataset(rng, 5, 20, 0.5);
  auto trained = make_content();
  trained->fit(ds, Config::object(), TrainContext{});
  ScratchDir dir;
  trained->save(dir.path());
  auto loaded = make_content();
  loaded->load(dir.path());
  CHECK_FALSE(loaded->supports_trained_scoring());
  std::vector<float> out(loaded->n_items());
  CHECK_THROWS_AS(loaded->score_trained_user(0, out), NotImplemented);
  CHECK_THROWS_AS(trained->score_trained_user(ds.n_users(), out), std::out_of_range);
}
