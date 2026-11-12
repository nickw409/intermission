// Regression tests for audit finding SA2-1: row-offset (CSR indptr) arrays read from disk must be
// fully validated (size n+1, first 0, last nnz, non-decreasing) before any row is walked.
// Each input below has offsets [0, 3, 1] with nnz = 1: row 0 claims three entries but only one
// exists. Before the fix the loaders read past the end of the index array (ASan heap overflow);
// now each must fail cleanly, and the message shows that the whole-array offset check fired (so
// the tests fail without the fix even in a build without ASan).
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../../../core/src/sparse/offsets.hpp"
#include "../data/temp_dir.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"
#include "recs/sha256.hpp"

namespace {

using Catch::Matchers::ContainsSubstring;
using recs::testing::TempDir;

struct Bytes {
  std::string s;
  void raw(const void* p, std::size_t n) { s.append(static_cast<const char*>(p), n); }
  void u8(std::uint8_t v) { raw(&v, 1); }
  void u32(std::uint32_t v) { raw(&v, 4); }
  void u64(std::uint64_t v) { raw(&v, 8); }
  void i64(std::int64_t v) { raw(&v, 8); }
  void f32(float v) { raw(&v, 4); }
  void f64(double v) { raw(&v, 8); }
  void seal() {
    recs::Sha256 sha;
    sha.update(std::string_view(s));
    const auto d = sha.digest();
    raw(d.data(), d.size());
  }
};

}  // namespace

TEST_CASE("SA2-1: knn load rejects non-monotonic row offsets before walking rows", "[security][sa2]") {
  TempDir dir("sa2-knn");
  Bytes b;
  b.raw("RECSKNN1", 8);
  b.u32(2);            // format version
  b.u32(2);            // n_items
  b.u32(0xFFFFFFFFu);  // neighbors
  b.u32(1);            // min_support
  b.f64(100.0);        // shrinkage
  b.u8(1);             // normalize
  b.u8(0);             // positive_only
  b.u64(1);            // nnz
  b.u64(0);            // row offsets [0, 3, 1]
  b.u64(3);
  b.u64(1);
  b.u32(1);     // items[0]
  b.f32(0.9f);  // sims[0]
  dir.write("item_knn.bin", b.s);
  auto m = recs::make_model("knn");
  CHECK_THROWS_WITH(m->load(dir.path()), ContainsSubstring("row offsets: decrease at row 1"));
}

TEST_CASE("SA2-1: knn format 3 load rejects non-monotonic row offsets before walking rows", "[security][sa2]") {
  TempDir dir("sa2-knn3");
  Bytes b;
  b.raw("RECSKNN1", 8);
  b.u32(3);            // format version
  b.u32(2);            // n_items
  b.u32(0xFFFFFFFFu);  // neighbors
  b.u32(1);            // min_support
  b.f64(100.0);        // shrinkage
  b.u8(1);             // normalize
  b.u8(0);             // positive_only
  b.f64(20.0);         // mean_damping
  b.f64(3.5);          // global mean
  b.u64(1);            // nnz
  b.u64(0);            // row offsets [0, 3, 1]
  b.u64(3);
  b.u64(1);
  b.u32(1);     // items[0]
  b.f32(0.9f);  // sims[0]
  dir.write("item_knn.bin", b.s);
  auto m = recs::make_model("knn");
  CHECK_THROWS_WITH(m->load(dir.path()), ContainsSubstring("row offsets: decrease at row 1"));
}

TEST_CASE("SA2-1: content load rejects non-monotonic row offsets before walking rows", "[security][sa2]") {
  TempDir dir("sa2-content");
  const auto cfg = recs::make_model("content")->config();
  dir.write("config.json", cfg.dump());
  Bytes b;
  b.raw("RECSCNT1", 8);
  b.u32(1);    // format version
  b.u32(2);    // n_items
  b.u32(100);  // n_features
  b.u32(100);  // n_terms
  b.u32(0);    // n_genres
  b.u32(0);    // flags
  b.u64(1);    // nnz
  b.u32(static_cast<std::uint32_t>(cfg.at("min_tag_count").get<std::int64_t>()));
  b.f64(cfg.at("genre_weight").get<double>());
  b.u64(0);  // indptr [0, 3, 1]
  b.u64(3);
  b.u64(1);
  b.u32(5);     // indices[0]
  b.f32(1.0f);  // values[0]
  b.seal();
  dir.write("features.bin", b.s);
  auto m = recs::make_model("content");
  CHECK_THROWS_WITH(m->load(dir.path()), ContainsSubstring("row offsets: decrease at row 1"));
}

TEST_CASE("SA2-1: dataset cache rejects non-monotonic row offsets before walking rows", "[security][sa2]") {
  TempDir dir("sa2-cache");
  Bytes b;
  b.raw("RECSDS01", 8);
  b.u32(2);  // format version
  b.u32(0);  // flags
  b.u32(2);  // n_users
  b.u32(8);  // n_items
  b.u64(1);  // nnz
  b.u64(0);  // n_tags
  b.u64(1);  // name
  b.raw("x", 1);
  b.u64(0);  // n_sources
  b.i64(1);  // user ids
  b.i64(2);
  for (int i = 0; i < 8; ++i) b.i64(100 + i);  // item ids
  b.u64(0);  // indptr [0, 3, 1]
  b.u64(3);
  b.u64(1);
  b.u32(0);                       // indices
  b.f32(4.0f);                    // values
  b.i64(0);                       // ts
  b.s += std::string(400, '\0');  // padding so later reads stay in bounds
  b.seal();
  dir.write("ds.bin", b.s);
  CHECK_THROWS_WITH(recs::load_cache(dir / "ds.bin"), ContainsSubstring("row offsets: decrease at row 1"));
}

TEST_CASE("check_row_offsets: every rule, and no wrap-around on n_rows + 1", "[security][sa2]") {
  using recs::detail::check_row_offsets;
  const std::vector<std::uint64_t> good{0, 2, 2, 5};
  CHECK_FALSE(check_row_offsets(good, 3, 5).has_value());
  CHECK(check_row_offsets(std::vector<std::uint64_t>{0}, 0, 0) == std::nullopt);
  CHECK(check_row_offsets(good, 2, 5).has_value());                                     // size
  CHECK(check_row_offsets(std::vector<std::uint64_t>{1, 2, 2, 5}, 3, 5).has_value());  // first
  CHECK(check_row_offsets(good, 3, 4).has_value());                                     // last
  CHECK(check_row_offsets(std::vector<std::uint64_t>{0, 3, 1, 5}, 3, 5).has_value());  // decrease
  // n_rows = 2^64 - 1 made n_rows + 1 wrap to 0, so an empty array passed the size check and
  // front() read from an empty span.
  CHECK(check_row_offsets(std::span<const std::uint64_t>{}, ~std::uint64_t{0}, 0).has_value());
  CHECK(check_row_offsets(std::span<const std::uint64_t>{}, 0, 0).has_value());
}
