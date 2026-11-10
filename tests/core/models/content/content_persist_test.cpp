// Content model (brief E gate 6): results do not depend on the thread count, save/load round-trips
// to bit-identical scores, and load() rejects damaged or hostile files.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <omp.h>

#include <catch2/catch_test_macros.hpp>

#include "content_fixtures.hpp"
#include "property.hpp"
#include "recs/model.hpp"
#include "recs/sha256.hpp"

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
  const ItemIdx stride = std::max<ItemIdx>(1, ds.n_items() / 60);
  for (ItemIdx i = 0; i < ds.n_items(); i += stride) o.similar.push_back(m.similar(i, 10));
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
  // The second size is above the catalog size at which score() starts a thread team.
  recs::testing::for_seeds(2, 5150, [](Rng& rng, std::uint64_t) {
    for (std::uint32_t n_items : {80u, 6000u}) {
      INFO("n_items = " << n_items);
      const auto ds = random_dataset(rng, 12, n_items, 40.0 / n_items);
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
    }
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
  // (Header and structure checks behind a valid checksum are tested below, with the file resealed.)
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

namespace {

// features.bin layout, from the format comment in content.cpp: a 52-byte header
//   0 magic[8] | 8 u32 version | 12 u32 n_items | 16 u32 n_features | 20 u32 n_terms
//   24 u32 n_genres | 28 u32 flags | 32 u64 nnz | 40 u32 min_tag_count | 44 f64 genre_weight
// then indptr (u64 x n_items+1), indices (u32 x nnz), values (f32 x nnz), SHA-256 (32 bytes).
struct Layout {
  std::uint32_t n_items = 0, n_features = 0, n_terms = 0, n_genres = 0;
  std::uint64_t nnz = 0;
  std::size_t indptr_at = 52, indices_at = 0, values_at = 0;
};

template <class T>
T get(const std::string& b, std::size_t at) {
  T v;
  std::memcpy(&v, b.data() + at, sizeof(T));
  return v;
}
template <class T>
void put(std::string& b, std::size_t at, T v) {
  std::memcpy(b.data() + at, &v, sizeof(T));
}

Layout layout_of(const std::string& b) {
  Layout l;
  l.n_items = get<std::uint32_t>(b, 12);
  l.n_features = get<std::uint32_t>(b, 16);
  l.n_terms = get<std::uint32_t>(b, 20);
  l.n_genres = get<std::uint32_t>(b, 24);
  l.nnz = get<std::uint64_t>(b, 32);
  l.indices_at = l.indptr_at + (static_cast<std::size_t>(l.n_items) + 1) * 8;
  l.values_at = l.indices_at + l.nnz * 4;
  REQUIRE(l.values_at + l.nnz * 4 + 32 == b.size());
  return l;
}

/// Recomputes the trailing SHA-256 so that a corruption gets past the checksum and reaches the
/// structural checks.
void reseal(std::string& b) {
  Sha256 sha;
  sha.update(std::string_view(b.data(), b.size() - 32));
  const auto d = sha.digest();
  std::memcpy(b.data() + b.size() - 32, d.data(), 32);
}

}  // namespace

TEST_CASE("Content load: structurally invalid files with a valid checksum are rejected", "[content][persist]") {
  Rng rng(31337);
  const auto ds = random_dataset(rng, 10, 40, 0.3);
  auto trained = make_content();
  trained->fit(ds, Config::object(), TrainContext{});
  ScratchDir dir;
  trained->save(dir.path());
  const auto bin = dir.path() / "features.bin";
  const auto cfg = dir.path() / "config.json";
  const std::string good = read_file(bin);
  const Layout l = layout_of(good);
  REQUIRE(l.nnz >= 2);
  REQUIRE(l.n_genres > 0);  // the genome-flag case relies on genre features being present

  // A loaded model that every failed load() below must leave untouched.
  auto target = make_content();
  target->load(dir.path());
  const auto state = target->fold_in(user_ratings(ds, 0));
  const auto before = scores_of(*target, state);

  std::uint32_t max_index = 0;
  for (std::uint64_t k = 0; k < l.nnz; ++k) max_index = std::max(max_index, get<std::uint32_t>(good, l.indices_at + 4 * k));
  std::uint32_t row_with_two = l.n_items;
  for (std::uint32_t r = 0; r < l.n_items && row_with_two == l.n_items; ++r)
    if (get<std::uint64_t>(good, l.indptr_at + 8 * (r + 1)) - get<std::uint64_t>(good, l.indptr_at + 8 * r) >= 2)
      row_with_two = r;
  REQUIRE(row_with_two < l.n_items);
  const auto row_begin = get<std::uint64_t>(good, l.indptr_at + 8 * row_with_two);
  const auto row_end = get<std::uint64_t>(good, l.indptr_at + 8 * (row_with_two + 1));

  using Corrupt = std::function<void(std::string&)>;
  const std::vector<std::pair<std::string, Corrupt>> cases{
      {"feature index == n_features", [&](std::string& b) { put<std::uint32_t>(b, l.indices_at, l.n_features); }},
      {"feature index 0xFFFFFFFF", [&](std::string& b) { put<std::uint32_t>(b, l.indices_at + 4 * (l.nnz - 1), 0xFFFFFFFFu); }},
      {"feature indices not increasing within a row",
       [&](std::string& b) {
         const auto a = get<std::uint32_t>(b, l.indices_at + 4 * row_begin);
         const auto c = get<std::uint32_t>(b, l.indices_at + 4 * (row_begin + 1));
         put(b, l.indices_at + 4 * row_begin, c);
         put(b, l.indices_at + 4 * (row_begin + 1), a);
       }},
      {"n_features not above the largest index (n_terms + n_genres kept consistent)",
       [&](std::string& b) {
         const std::uint32_t nt = std::min(l.n_terms, max_index);
         put(b, 16, max_index);
         put(b, 20, nt);
         put(b, 24, max_index - nt);
       }},
      {"n_terms + n_genres != n_features", [&](std::string& b) { put<std::uint32_t>(b, 20, l.n_terms + 1); }},
      {"value 2.0", [&](std::string& b) { put(b, l.values_at, 2.0f); }},
      {"value 0", [&](std::string& b) { put(b, l.values_at, 0.0f); }},
      {"value negative", [&](std::string& b) { put(b, l.values_at, -0.25f); }},
      {"value NaN", [&](std::string& b) { put(b, l.values_at, std::numeric_limits<float>::quiet_NaN()); }},
      {"value infinity", [&](std::string& b) { put(b, l.values_at, std::numeric_limits<float>::infinity()); }},
      {"value 1e30", [&](std::string& b) { put(b, l.values_at + 4 * (l.nnz - 1), 1e30f); }},
      {"n_items + 1 (size mismatch)", [&](std::string& b) { put<std::uint32_t>(b, 12, l.n_items + 1); }},
      {"n_items 0xFFFFFFFF", [&](std::string& b) { put<std::uint32_t>(b, 12, 0xFFFFFFFFu); }},
      {"nnz + 1 (size mismatch)", [&](std::string& b) { put<std::uint64_t>(b, 32, l.nnz + 1); }},
      {"nnz 2^64 - 1", [&](std::string& b) { put<std::uint64_t>(b, 32, ~std::uint64_t{0}); }},
      {"indptr[0] != 0", [&](std::string& b) { put<std::uint64_t>(b, l.indptr_at, 1); }},
      {"indptr past nnz", [&](std::string& b) { put<std::uint64_t>(b, l.indptr_at + 8, l.nnz + 5); }},
      {"indptr decreasing", [&](std::string& b) { put<std::uint64_t>(b, l.indptr_at + 8 * row_with_two, row_end + 1); }},
      {"indptr end != nnz", [&](std::string& b) { put<std::uint64_t>(b, l.indptr_at + 8 * l.n_items, l.nnz - 1); }},
      {"unknown flag", [&](std::string& b) { put<std::uint32_t>(b, 28, 2); }},
      {"genome flag with genre features", [&](std::string& b) { put<std::uint32_t>(b, 28, 1); }},
      {"format version", [&](std::string& b) { put<std::uint32_t>(b, 8, 2); }},
      {"magic", [&](std::string& b) { b[0] = 'X'; }},
      {"header min_tag_count != config.json", [&](std::string& b) { put<std::uint32_t>(b, 40, 3); }},
      {"header genre_weight != config.json", [&](std::string& b) { put<double>(b, 44, 0.5); }},
  };
  for (const auto& [what, corrupt] : cases) {
    INFO(what);
    std::string bad = good;
    corrupt(bad);
    REQUIRE(bad != good);
    reseal(bad);
    write_file(bin, bad);
    CHECK_THROWS(target->load(dir.path()));
    // Nothing from the rejected file was committed: no bad index can reach scoring.
    CHECK(bitwise_equal(scores_of(*target, state), before));
  }
  write_file(bin, good);

  // A config.json that is valid on its own but does not match the feature file.
  const std::string good_cfg = read_file(cfg);
  write_file(cfg, R"({"genre_weight":1.0,"min_tag_count":3})");
  CHECK_THROWS(target->load(dir.path()));
  write_file(cfg, R"({"genre_weight":2.0,"min_tag_count":2})");
  CHECK_THROWS(target->load(dir.path()));
  write_file(cfg, good_cfg);
  CHECK_NOTHROW(target->load(dir.path()));
  CHECK(bitwise_equal(scores_of(*target, state), before));
}
