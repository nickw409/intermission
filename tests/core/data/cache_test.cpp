// Gate tests for the binary dataset cache (spec Data, ingest step 5): exact round-trip, and every
// corruption (any flipped byte, any truncation, a version change) is detected.
//
// Most tests rely only on the layout promised in recs/dataset.hpp: the file starts with the
// 8-byte magic "RECSDS01" followed by the format version as a little-endian uint32, and ends with
// the SHA-256 of everything before it. The "resealed" tests further down also use field offsets of
// format version 2 (documented in core/src/data/cache.cpp) and check them first.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "dataset_eq.hpp"
#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/sha256.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;
using recs::testing::TempDir;

namespace {

std::string read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_bytes(const fs::path& p, const std::string& bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out);
}

// Replaces the trailing SHA-256 so that only the deliberate edit (not the checksum) is wrong.
std::string reseal(std::string bytes) {
  REQUIRE(bytes.size() > 32);
  bytes.resize(bytes.size() - 32);
  recs::Sha256 h;
  h.update(std::string_view(bytes));
  const auto d = h.digest();
  bytes.append(reinterpret_cast<const char*>(d.data()), d.size());
  return bytes;
}

// A small dataset that exercises every field: external ids that differ from dense ids, optional
// metadata both present and absent, UTF-8 and embedded separators, tags, and a genome.
recs::Dataset synthetic(bool with_genome) {
  std::vector<recs::MovieMeta> movies(5);
  movies[0] = {101, "Alpha", 1999, {"Drama", "Comedy"}, 133093, 603};
  movies[1] = {102, "", std::nullopt, {}, std::nullopt, std::nullopt};
  movies[2] = {205, "Am\xC3\xA9lie, \"quoted\"\nline", 2001, {"Romance"}, 211915, std::nullopt};
  movies[3] = {300, "Cold", -5, {"A", "", "B"}, std::nullopt, 7};
  movies[4] = {9000000000LL, "Big id", 2024, {"X"}, 1, 2};
  std::vector<recs::Triplet> ratings = {
      {0, 0, 4.5f, 1600000000}, {0, 2, 0.5f, -3}, {1, 1, 5.0f, 0},
      {2, 0, 3.0f, 1}, {2, 3, 2.5f, 9000000000LL}, {3, 2, 1.0f, 42},
  };
  auto ds = recs::make_dataset("synthetic", 4, 5, ratings, movies);
  recs::IdMap users;
  for (std::int64_t ext : {77, 3, 100000, 12}) users.insert(ext);
  ds.users = users;
  ds.tags = {{77, 0, "pixar", 10}, {999, 4, "", 11}, {3, 2, "comma, \"quote\"", 12}};
  if (with_genome) {
    recs::GenomeMatrix g;
    g.n_items = 5;
    g.n_tags = 3;
    g.tag_names = {"funny", "dark", "\xC3\xA9pique"};
    for (int i = 0; i < 15; ++i) g.relevance.push_back(static_cast<float>(i) / 16.0f + 0.001f);
    ds.genome = g;
  }
  return ds;
}

void require_cache_error(const fs::path& file) {
  CHECK_THROWS_AS(recs::load_cache(file), recs::DatasetError);
}

}  // namespace

TEST_CASE("cache round-trips a dataset exactly", "[cache]") {
  TempDir dir("cache-rt");
  SECTION("synthetic with genome") {
    const auto ds = synthetic(true);
    recs::save_cache(ds, dir / "a.bin");
    recs::testing::check_dataset_equal(recs::load_cache(dir / "a.bin"), ds);
  }
  SECTION("synthetic without genome") {
    const auto ds = synthetic(false);
    recs::save_cache(ds, dir / "b.bin");
    const auto back = recs::load_cache(dir / "b.bin");
    CHECK_FALSE(back.genome.has_value());
    recs::testing::check_dataset_equal(back, ds);
  }
  SECTION("empty dataset") {
    const auto ds = recs::make_dataset("", 0, 0, {});
    recs::save_cache(ds, dir / "c.bin");
    recs::testing::check_dataset_equal(recs::load_cache(dir / "c.bin"), ds);
  }
  SECTION("tiny fixture") {
    const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
    recs::save_cache(ds, dir / "nested" / "tiny.bin");  // parent directories are created
    recs::testing::check_dataset_equal(recs::load_cache(dir / "nested" / "tiny.bin"), ds);
  }
  SECTION("saving is deterministic") {
    const auto ds = synthetic(true);
    recs::save_cache(ds, dir / "d1.bin");
    recs::save_cache(ds, dir / "d2.bin");
    CHECK(read_bytes(dir / "d1.bin") == read_bytes(dir / "d2.bin"));
  }
}

TEST_CASE("cache header starts with the magic and version", "[cache]") {
  TempDir dir("cache-hdr");
  recs::save_cache(synthetic(true), dir / "a.bin");
  const auto bytes = read_bytes(dir / "a.bin");
  REQUIRE(bytes.size() > 8 + 4 + 32);
  CHECK(bytes.substr(0, 8) == "RECSDS01");
  // The trailer is the SHA-256 of everything before it.
  recs::Sha256 h;
  h.update(std::string_view(bytes).substr(0, bytes.size() - 32));
  const auto d = h.digest();
  CHECK(std::memcmp(d.data(), bytes.data() + bytes.size() - 32, 32) == 0);
}

TEST_CASE("cache: flipping any single byte is detected", "[cache]") {
  TempDir dir("cache-flip");
  recs::save_cache(synthetic(true), dir / "good.bin");
  const auto good = read_bytes(dir / "good.bin");
  const auto bad = dir / "bad.bin";
  for (std::size_t i = 0; i < good.size(); ++i) {
    for (unsigned char mask : {0x01, 0x80, 0xFF}) {
      INFO("byte " << i << " of " << good.size() << " xor " << int(mask));
      auto bytes = good;
      bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ mask);
      write_bytes(bad, bytes);
      require_cache_error(bad);
    }
  }
}

TEST_CASE("cache: every truncation and any trailing data is detected", "[cache]") {
  TempDir dir("cache-trunc");
  recs::save_cache(synthetic(true), dir / "good.bin");
  const auto good = read_bytes(dir / "good.bin");
  const auto bad = dir / "bad.bin";
  for (std::size_t len = 0; len < good.size(); ++len) {
    INFO("truncated to " << len << " of " << good.size());
    write_bytes(bad, good.substr(0, len));
    require_cache_error(bad);
  }
  write_bytes(bad, good + std::string(1, '\0'));
  require_cache_error(bad);
  write_bytes(bad, reseal(good + std::string(32, '\0')));  // extra payload with a valid checksum
  require_cache_error(bad);
}

TEST_CASE("cache: a version or magic change is rejected even with a valid checksum", "[cache]") {
  TempDir dir("cache-ver");
  recs::save_cache(synthetic(true), dir / "good.bin");
  const auto good = read_bytes(dir / "good.bin");
  const auto bad = dir / "bad.bin";

  std::uint32_t version = 0;
  std::memcpy(&version, good.data() + 8, 4);
  for (std::uint32_t v : {version + 1, version - 1, 0u, 0xFFFFFFFFu}) {
    INFO("version " << v);
    auto bytes = good;
    std::memcpy(bytes.data() + 8, &v, 4);
    write_bytes(bad, reseal(bytes));
    try {
      (void)recs::load_cache(bad);
      FAIL("expected DatasetError");
    } catch (const recs::DatasetError& e) {
      CHECK(std::string(e.what()).find("version") != std::string::npos);
    }
  }
  auto bytes = good;
  bytes[7] = '2';  // "RECSDS02"
  write_bytes(bad, reseal(bytes));
  require_cache_error(bad);
}

TEST_CASE("cache: missing file is a DatasetError", "[cache]") {
  TempDir dir("cache-missing");
  require_cache_error(dir / "nope.bin");
}

// --- Resealed edits ------------------------------------------------------------------------------
// The tests below edit one value inside the payload and then recompute the trailing SHA-256, so the
// checksum cannot be what rejects the file: the loader's own checks must. They rely on the field
// offsets of format version 2 for the `synthetic` dataset (see core/src/data/cache.cpp); a separate
// test checks the expected value sits at each offset, so a layout change fails loudly.
namespace {

constexpr std::size_t kOffNUsers = 16;   // u32
constexpr std::size_t kOffNnz = 24;      // u64
constexpr std::size_t kOffNTags = 32;    // u64 (tag applications)
constexpr std::size_t kOffNameLen = 40;  // u64, then "synthetic" (9 bytes)
constexpr std::size_t kOffSources = 57;  // u64 count of raw-file fingerprints (0 from save_cache)
constexpr std::size_t kOffValues = 65 + 4 * 8 + 5 * 8 + 5 * 8 + 6 * 4;  // after users, items, indptr, indices

template <class T>
T peek(const std::string& bytes, std::size_t off) {
  T v{};
  REQUIRE(off + sizeof v <= bytes.size());
  std::memcpy(&v, bytes.data() + off, sizeof v);
  return v;
}

template <class T>
std::string patched(std::string bytes, std::size_t off, T v) {
  REQUIRE(off + sizeof v <= bytes.size());
  std::memcpy(bytes.data() + off, &v, sizeof v);
  return reseal(bytes);
}

recs::Dataset synthetic_genome_without_tags() {
  auto ds = synthetic(false);
  recs::GenomeMatrix g;
  g.n_items = 5;
  g.n_tags = 0;
  ds.genome = g;
  return ds;
}

void require_resealed_error(const TempDir& dir, const std::string& bytes, const std::string& what) {
  write_bytes(dir / "bad.bin", bytes);
  CHECK_THROWS_MATCHES(recs::load_cache(dir / "bad.bin"), recs::DatasetError, MessageMatches(ContainsSubstring(what)));
}

void copy_tiny(const fs::path& to) {
  fs::create_directories(to);
  for (const char* f : {"movies.csv", "links.csv", "ratings.csv", "tags.csv"}) {
    fs::copy_file(recs::testing::tiny_dir() / f, to / f);
  }
}

}  // namespace

TEST_CASE("cache: the layout offsets used by the resealed tests are right", "[cache]") {
  TempDir dir("cache-layout");
  recs::save_cache(synthetic(true), dir / "a.bin");
  const auto good = read_bytes(dir / "a.bin");
  CHECK(peek<std::uint32_t>(good, kOffNUsers) == 4);
  CHECK(peek<std::uint64_t>(good, kOffNnz) == 6);
  CHECK(peek<std::uint64_t>(good, kOffNTags) == 3);
  CHECK(peek<std::uint64_t>(good, kOffNameLen) == 9);
  CHECK(good.substr(kOffNameLen + 8, 9) == "synthetic");
  CHECK(peek<std::uint64_t>(good, kOffSources) == 0);
  CHECK(peek<float>(good, kOffValues) == 4.5f);
  CHECK(peek<float>(good, kOffValues + 4) == 0.5f);
  CHECK(reseal(good) == good);  // resealing an unmodified file reproduces it exactly
}

TEST_CASE("cache: genome dimensions must match the item count", "[cache]") {
  TempDir dir("cache-genome-dims");
  SECTION("load rejects a resealed genome n_items") {
    recs::save_cache(synthetic_genome_without_tags(), dir / "a.bin");
    const auto good = read_bytes(dir / "a.bin");
    const std::size_t off = good.size() - 32 - 8;  // genome block: u32 n_items, u32 n_tags (= 0)
    REQUIRE(peek<std::uint32_t>(good, off) == 5);
    REQUIRE(peek<std::uint32_t>(good, off + 4) == 0);
    for (std::uint32_t n : {4u, 6u, 1000000u}) {
      INFO("genome n_items " << n);
      require_resealed_error(dir, patched(good, off, n), "genome");
    }
  }
  SECTION("save rejects inconsistent genomes and leaves no file behind") {
    auto ds = synthetic(true);
    ds.genome->n_items = 6;
    ds.genome->relevance.resize(18, 0.0f);
    CHECK_THROWS_AS(recs::save_cache(ds, dir / "x.bin"), recs::DatasetError);
    ds = synthetic(true);
    ds.genome->tag_names.pop_back();
    CHECK_THROWS_AS(recs::save_cache(ds, dir / "x.bin"), recs::DatasetError);
    ds = synthetic(true);
    ds.genome->relevance.pop_back();
    CHECK_THROWS_AS(recs::save_cache(ds, dir / "x.bin"), recs::DatasetError);
    CHECK(fs::is_empty(dir.path()));
  }
}

TEST_CASE("cache: out-of-range ratings and relevance are rejected", "[cache]") {
  TempDir dir("cache-ranges");
  recs::save_cache(synthetic(true), dir / "a.bin");
  const auto good = read_bytes(dir / "a.bin");
  REQUIRE(peek<float>(good, kOffValues) == 4.5f);
  for (float v : {0.0f, 0.25f, 5.5f, 3.25f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::infinity()}) {
    INFO("rating " << v);
    require_resealed_error(dir, patched(good, kOffValues, v), "rating");
  }
  const std::size_t rel = good.size() - 32 - 15 * 4;  // relevance is the last array of the payload
  REQUIRE(peek<float>(good, rel) == 0.001f);
  for (float v : {1.5f, -0.1f, std::numeric_limits<float>::quiet_NaN()}) {
    INFO("relevance " << v);
    require_resealed_error(dir, patched(good, rel, v), "relevance");
  }
}

TEST_CASE("cache: hostile lengths are rejected without huge allocations", "[cache]") {
  TempDir dir("cache-hostile");
  recs::save_cache(synthetic(true), dir / "a.bin");
  const auto good = read_bytes(dir / "a.bin");
  for (std::uint64_t n : {std::uint64_t{1} << 40, std::uint64_t{1} << 62, ~std::uint64_t{0}}) {
    INFO("length " << n);
    require_resealed_error(dir, patched(good, kOffNameLen, n), "invalid dataset cache");
    require_resealed_error(dir, patched(good, kOffNnz, n), "invalid dataset cache");
    require_resealed_error(dir, patched(good, kOffNTags, n), "invalid dataset cache");
    require_resealed_error(dir, patched(good, kOffSources, n), "invalid dataset cache");
  }
  require_resealed_error(dir, patched(good, kOffNUsers, std::uint32_t{0xFFFFFFFF}), "invalid dataset cache");
}

TEST_CASE("save_cache validates before writing anything", "[cache]") {
  TempDir dir("cache-save-validate");
  auto bad_rating = synthetic(false);
  bad_rating.by_user.values[0] = 7.0f;
  CHECK_THROWS_AS(recs::save_cache(bad_rating, dir / "a.bin"), recs::DatasetError);
  auto bad_csr = synthetic(false);
  bad_csr.by_user.indices[1] = bad_csr.by_user.indices[0];
  CHECK_THROWS_AS(recs::save_cache(bad_csr, dir / "a.bin"), recs::DatasetError);
  auto bad_movie = synthetic(false);
  bad_movie.movies[1].movie_id = 4242;
  CHECK_THROWS_AS(recs::save_cache(bad_movie, dir / "a.bin"), recs::DatasetError);
  auto bad_tag = synthetic(false);
  bad_tag.tags[0].item = 5;
  CHECK_THROWS_AS(recs::save_cache(bad_tag, dir / "a.bin"), recs::DatasetError);
  CHECK(fs::is_empty(dir.path()));

  // A failed save never clobbers an existing good cache or leaves a temp file behind.
  recs::save_cache(synthetic(false), dir / "a.bin");
  const auto before = read_bytes(dir / "a.bin");
  CHECK_THROWS_AS(recs::save_cache(bad_rating, dir / "a.bin"), recs::DatasetError);
  CHECK(read_bytes(dir / "a.bin") == before);
  CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 1);
}

TEST_CASE("load_or_build builds, reuses a valid cache and rebuilds a corrupt one", "[cache]") {
  TempDir dir("cache-lob");
  const auto raw = dir / "raw";
  copy_tiny(raw);
  const auto cache = dir / "sub" / "tiny.bin";
  const auto expected = recs::load_movielens(raw, "tiny");

  // 1. No cache yet: parses the raw files and writes the cache; a second call reuses it.
  REQUIRE_FALSE(fs::exists(cache));
  recs::testing::check_dataset_equal(recs::load_or_build(raw, cache, "tiny"), expected);
  REQUIRE(fs::exists(cache));
  recs::testing::check_dataset_equal(recs::load_cache(cache), expected);
  recs::testing::check_dataset_equal(recs::load_or_build(raw, cache, "tiny"), expected);

  // 2. Corrupt cache: rebuilt from the raw files and rewritten.
  auto bytes = read_bytes(cache);
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x10);
  write_bytes(cache, bytes);
  require_cache_error(cache);
  recs::testing::check_dataset_equal(recs::load_or_build(raw, cache, "tiny"), expected);
  recs::testing::check_dataset_equal(recs::load_cache(cache), expected);

  // 3. Corrupt cache and no raw data: the parse error surfaces instead of the bad cache.
  write_bytes(cache, "garbage");
  CHECK_THROWS_MATCHES(recs::load_or_build(dir / "no-raw-here", cache, "tiny"), recs::DatasetError,
                       MessageMatches(ContainsSubstring("movies.csv")));

  // 4. A cache built under another dataset name is not reused.
  recs::save_cache(recs::make_dataset("other", 1, 1, {{0, 0, 3.0f, 1}}), cache);
  recs::testing::check_dataset_equal(recs::load_or_build(raw, cache, "tiny"), expected);

  // 5. A cache written by plain save_cache records no raw files, so load_or_build rebuilds it.
  recs::save_cache(expected, cache);
  const auto plain = read_bytes(cache);
  recs::testing::check_dataset_equal(recs::load_or_build(raw, cache, "tiny"), expected);
  CHECK(read_bytes(cache) != plain);
}

TEST_CASE("load_or_build rebuilds when a raw file changes under the same name", "[cache]") {
  TempDir dir("cache-stale");
  const auto raw = dir / "raw";
  copy_tiny(raw);
  const auto cache = dir / "tiny.bin";
  const auto ratings = raw / "ratings.csv";
  // User 1's first ratings.csv row is "1,10,0.5,..."; read that rating back from a dataset.
  const auto user1_movie10 = [](const recs::Dataset& ds) {
    const auto u = *ds.users.find(1);
    const auto i = *ds.items.find(10);
    const auto cols = ds.by_user.row_indices(u);
    for (std::size_t k = 0; k < cols.size(); ++k) {
      if (cols[k] == i) return ds.by_user.row_values(u)[k];
    }
    FAIL("rating (1, 10) missing");
    return 0.0f;
  };
  REQUIRE(user1_movie10(recs::load_or_build(raw, cache, "tiny")) == 0.5f);
  const auto original_mtime = fs::last_write_time(ratings);

  // Same size and same mtime: the cache is trusted (proved by an edit the cache cannot know about).
  auto text = read_bytes(ratings);
  const auto at = text.find("1,10,0.5,");
  REQUIRE(at != std::string::npos);
  text.replace(at, 9, "1,10,1.0,");
  write_bytes(ratings, text);
  fs::last_write_time(ratings, original_mtime);
  CHECK(user1_movie10(recs::load_or_build(raw, cache, "tiny")) == 0.5f);

  // Same size, different mtime: rebuilt, and the new cache is then reused.
  fs::last_write_time(ratings, original_mtime + std::chrono::seconds(1));
  CHECK(user1_movie10(recs::load_or_build(raw, cache, "tiny")) == 1.0f);
  CHECK(user1_movie10(recs::load_or_build(raw, cache, "tiny")) == 1.0f);

  // Different size with the mtime forced back: rebuilt.
  const auto mtime = fs::last_write_time(ratings);
  write_bytes(ratings, text + "21,1,4.0,5\n");
  fs::last_write_time(ratings, mtime);
  CHECK(recs::load_or_build(raw, cache, "tiny").n_ratings() == 246);

  // An optional file disappearing or appearing is a change too.
  fs::remove(raw / "tags.csv");
  CHECK(recs::load_or_build(raw, cache, "tiny").tags.empty());
  fs::copy_file(recs::testing::tiny_dir() / "tags.csv", raw / "tags.csv");
  CHECK(recs::load_or_build(raw, cache, "tiny").tags.size() == 11);
}
