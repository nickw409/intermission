// Gate tests for the binary dataset cache (spec Data, ingest step 5): exact round-trip, and every
// corruption (any flipped byte, any truncation, a version change) is detected.
//
// The test relies only on the layout promised in recs/dataset.hpp: the file starts with the
// 8-byte magic "RECSDS01" followed by the format version as a little-endian uint32, and ends with
// the SHA-256 of everything before it.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dataset_eq.hpp"
#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/sha256.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
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

TEST_CASE("load_or_build builds, reuses a valid cache and rebuilds a corrupt one", "[cache]") {
  TempDir dir("cache-lob");
  const auto cache = dir / "sub" / "tiny.bin";
  const auto expected = recs::load_movielens(recs::testing::tiny_dir(), "tiny");

  // 1. No cache yet: parses the raw files and writes the cache.
  REQUIRE_FALSE(fs::exists(cache));
  recs::testing::check_dataset_equal(recs::load_or_build(recs::testing::tiny_dir(), cache, "tiny"), expected);
  REQUIRE(fs::exists(cache));
  recs::testing::check_dataset_equal(recs::load_cache(cache), expected);

  // 2. Valid cache: used as is. The raw directory does not exist, so parsing would throw.
  recs::testing::check_dataset_equal(recs::load_or_build(dir / "no-raw-here", cache, "tiny"), expected);

  // 3. Corrupt cache: rebuilt from the raw files and rewritten.
  auto bytes = read_bytes(cache);
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x10);
  write_bytes(cache, bytes);
  require_cache_error(cache);
  recs::testing::check_dataset_equal(recs::load_or_build(recs::testing::tiny_dir(), cache, "tiny"), expected);
  recs::testing::check_dataset_equal(recs::load_cache(cache), expected);

  // 4. Corrupt cache and no raw data: the parse error surfaces instead of the bad cache.
  write_bytes(cache, "garbage");
  CHECK_THROWS(recs::load_or_build(dir / "no-raw-here", cache, "tiny"));

  // 5. A cache built under another dataset name is not reused.
  recs::save_cache(recs::make_dataset("other", 1, 1, {{0, 0, 3.0f, 1}}), cache);
  recs::testing::check_dataset_equal(recs::load_or_build(recs::testing::tiny_dir(), cache, "tiny"), expected);
}
