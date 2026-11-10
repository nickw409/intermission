// Regression tests for audit finding SA2-5: CURRENT and manifest.json are read with hard limits.
// CURRENT must be a regular file (a symlink is not followed) of at most 256 bytes; manifest.json
// must be a regular file of at most 4 MiB. Before the fix both were read whole, whatever they were.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../data/temp_dir.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;
using recs::testing::TempDir;

namespace {

fs::path write_small_artifact(const fs::path& root, const std::string& version) {
  std::vector<recs::MovieMeta> movies(2);
  movies[0] = {1, "A", 2000, {"Drama"}, std::nullopt, std::nullopt};
  movies[1] = {2, "B", 2001, {"Comedy"}, std::nullopt, std::nullopt};
  const auto ds = recs::make_dataset("sa2-5", 2, 2, {{0, 0, 4.f, 1}, {1, 1, 3.f, 2}, {1, 0, 5.f, 3}}, movies);
  auto pop = recs::make_popularity();
  pop->fit(ds, recs::Config{{"m", 1}}, {});
  return recs::write_artifact(root, version, recs::make_catalog(ds), {pop.get()}, "pop", "abc1234", "sa2-5");
}

}  // namespace

TEST_CASE("SA2-5: a symlinked CURRENT is not followed", "[security][sa2][artifact]") {
  TempDir root("sa2-5-symlink");
  root.write("elsewhere", "v1\n");
  fs::create_symlink(root / "elsewhere", root / "CURRENT");
  CHECK_THROWS_AS(recs::read_current(root.path()), recs::ArtifactError);
}

TEST_CASE("SA2-5: a dangling CURRENT symlink is an error, not 'no CURRENT'", "[security][sa2][artifact]") {
  TempDir root("sa2-5-dangling");
  fs::create_symlink(root / "missing", root / "CURRENT");
  CHECK_THROWS_AS(recs::read_current(root.path()), recs::ArtifactError);
}

TEST_CASE("SA2-5: CURRENT must be a regular file", "[security][sa2][artifact]") {
  TempDir root("sa2-5-dir");
  fs::create_directory(root / "CURRENT");
  CHECK_THROWS_MATCHES(recs::read_current(root.path()), recs::ArtifactError,
                       MessageMatches(ContainsSubstring("not a regular file")));
}

TEST_CASE("SA2-5: an oversized CURRENT is rejected before it is read", "[security][sa2][artifact]") {
  TempDir root("sa2-5-big");
  root.write("CURRENT", std::string(257, 'a'));
  CHECK_THROWS_MATCHES(recs::read_current(root.path()), recs::ArtifactError,
                       MessageMatches(ContainsSubstring("larger than")));
  // The limit leaves room for any valid version (at most 100 characters plus a newline).
  root.write("CURRENT", std::string(100, 'a') + "\n");
  CHECK(recs::read_current(root.path()) == std::string(100, 'a'));
}

TEST_CASE("SA2-5: an oversized manifest.json is rejected", "[security][sa2][artifact]") {
  TempDir root("sa2-5-manifest");
  const fs::path dir = write_small_artifact(root.path(), "v1");
  REQUIRE_NOTHROW(recs::load_artifact(dir));
  // Trailing whitespace keeps the manifest valid JSON, so only the size limit can reject it.
  {
    std::ofstream out(dir / "manifest.json", std::ios::binary | std::ios::app);
    out << std::string(5u << 20, ' ');
  }
  CHECK_THROWS_MATCHES(recs::load_artifact(dir), recs::ArtifactError,
                       MessageMatches(ContainsSubstring("larger than")));
}

TEST_CASE("SA2-5: a symlinked manifest.json is rejected", "[security][sa2][artifact]") {
  TempDir root("sa2-5-manifest-link");
  const fs::path dir = write_small_artifact(root.path(), "v1");
  fs::rename(dir / "manifest.json", root / "real-manifest.json");
  fs::create_symlink(root / "real-manifest.json", dir / "manifest.json");
  CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
}
