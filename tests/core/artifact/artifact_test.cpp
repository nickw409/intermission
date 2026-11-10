// Gate 3 (C1): catalog, artifact write/load/verify, and the CURRENT pointer.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "../report/test_util.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace fs = std::filesystem;
using recs::testing::TempDir;

namespace {

std::vector<recs::MovieMeta> movies() {
  std::vector<recs::MovieMeta> m(4);
  m[0] = {101, "Toy Story", 1995, {"Animation", "Comedy"}, 114709, 862};
  m[1] = {205, "Amélie, \"Le\"", 2001, {"Romance"}, 211915, std::nullopt};
  m[2] = {7, "Hamlet", std::nullopt, {}, std::nullopt, std::nullopt};
  m[3] = {99999, "Never Rated", 2020, {"Drama"}, std::nullopt, 5};
  return m;
}

// Same ratings as the popularity fixture: counts {3, 2, 1, 0}, means {4, 3.5, 2, -}.
recs::Dataset fixture() {
  return recs::make_dataset("art-fixture", 3, 4,
                            {{0, 0, 5.f, 1}, {0, 1, 3.f, 2}, {1, 0, 4.f, 3},
                             {1, 2, 2.f, 4}, {2, 0, 3.f, 5}, {2, 1, 4.f, 6}},
                            movies());
}

struct Fitted {
  std::unique_ptr<recs::Model> pop = recs::make_popularity();
  std::unique_ptr<recs::Model> count = recs::make_popularity_count();
  Fitted() {
    const auto ds = fixture();
    pop->fit(ds, recs::Config{{"m", 5}}, {});
    count->fit(ds, recs::Config::object(), {});
  }
  std::vector<const recs::Model*> list() const { return {pop.get(), count.get()}; }
};

std::vector<float> scores(const recs::Model& m) {
  std::vector<float> out(m.n_items());
  m.score(m.fold_in({}), out);
  return out;
}

fs::path write_sample(const fs::path& root, const std::string& version = "2026-11-10T00-00-00Z") {
  const Fitted f;
  return recs::write_artifact(root, version, recs::make_catalog(fixture()), f.list(), "pop", "abc1234",
                              "art-fixture");
}

std::vector<fs::path> regular_files(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file()) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST_CASE("make_catalog: counts, means and popularity ranks", "[c1][artifact][catalog]") {
  const auto c = recs::make_catalog(fixture());
  CHECK(c.items == fixture().items);
  CHECK(c.movies == movies());
  CHECK(c.rating_count == std::vector<std::uint32_t>{3, 2, 1, 0});
  CHECK(c.mean_rating == std::vector<float>{4.f, 3.5f, 2.f, 0.f});
  CHECK(c.popularity_rank == std::vector<std::uint32_t>{1, 2, 3, 4});

  // Ties broken by lower ItemIdx.
  const auto tied = recs::make_catalog(recs::make_dataset("t", 2, 3, {{0, 2, 4.f, 0}, {1, 1, 3.f, 0}}));
  CHECK(tied.popularity_rank == std::vector<std::uint32_t>{3, 1, 2});
}

TEST_CASE("artifact: write -> load round-trip", "[c1][artifact]") {
  TempDir root("art-rt");
  const Fitted f;
  const auto catalog = recs::make_catalog(fixture());
  const auto dir = recs::write_artifact(root.path(), "2026-11-10T00-00-00Z", catalog, f.list(), "pop",
                                        "abc1234", "art-fixture");
  CHECK(dir == root.path() / "2026-11-10T00-00-00Z");
  CHECK(fs::exists(dir / "manifest.json"));

  // No temporary directory is left behind.
  for (const auto& e : fs::directory_iterator(root.path())) {
    CHECK(e.path().filename().string().rfind(".tmp-", 0) == std::string::npos);
  }

  const auto loaded = recs::load_artifact(dir);
  const auto& man = loaded.manifest;
  CHECK(man.format_version == 1);
  CHECK(man.version == "2026-11-10T00-00-00Z");
  CHECK(man.git_sha == "abc1234");
  CHECK(man.dataset == "art-fixture");
  CHECK(man.models == std::vector<std::string>{"pop", "pop_count"});
  CHECK(man.default_model == "pop");
  CHECK(man.config.at("pop") == f.pop->config());
  CHECK(man.config.at("pop_count") == f.count->config());
  CHECK(man.created_at.size() == 20);
  CHECK_FALSE(man.files.empty());
  for (const auto& [path, sha] : man.files) {
    CHECK(path.find('\\') == std::string::npos);
    CHECK(sha.size() == 64);
  }

  CHECK(loaded.catalog.items == catalog.items);
  CHECK(loaded.catalog.movies == catalog.movies);
  CHECK(loaded.catalog.rating_count == catalog.rating_count);
  CHECK(loaded.catalog.mean_rating == catalog.mean_rating);
  CHECK(loaded.catalog.popularity_rank == catalog.popularity_rank);

  REQUIRE(loaded.models.size() == 2);
  CHECK(scores(*loaded.models.at("pop")) == scores(*f.pop));
  CHECK(scores(*loaded.models.at("pop_count")) == scores(*f.count));
  CHECK(loaded.models.at("pop")->config() == f.pop->config());

  // manifest.json is sorted JSON listing every other file.
  const auto files = regular_files(dir);
  CHECK(files.size() == man.files.size() + 1);
}

TEST_CASE("artifact: writing is deterministic apart from created_at", "[c1][artifact]") {
  TempDir a("art-det-a"), b("art-det-b");
  const auto da = write_sample(a.path());
  const auto db = write_sample(b.path());
  auto ma = nlohmann::json::parse(recs::testing::read_file(da / "manifest.json"));
  auto mb = nlohmann::json::parse(recs::testing::read_file(db / "manifest.json"));
  ma.erase("created_at");
  mb.erase("created_at");
  CHECK(ma == mb);
}

TEST_CASE("artifact: flipping a byte in any listed file fails verification", "[c1][artifact]") {
  TempDir root("art-flip");
  const auto dir = write_sample(root.path());
  const auto files = regular_files(dir);
  REQUIRE(files.size() >= 3);
  for (const auto& f : files) {
    if (f.filename() == "manifest.json") continue;
    INFO("file = " << f);
    const auto original = recs::testing::read_file(f);
    REQUIRE_FALSE(original.empty());
    for (std::size_t pos : {std::size_t{0}, original.size() / 2, original.size() - 1}) {
      auto bad = original;
      bad[pos] = static_cast<char>(bad[pos] ^ 0x01);
      recs::testing::write_file(f, bad);
      CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
    }
    recs::testing::write_file(f, original);
    CHECK_NOTHROW(recs::load_artifact(dir));
  }
}

TEST_CASE("artifact: a missing, extra or unlisted file fails verification", "[c1][artifact]") {
  TempDir root("art-files");
  const auto dir = write_sample(root.path());
  const auto files = regular_files(dir);

  SECTION("deleting any file") {
    for (const auto& f : files) {
      INFO("file = " << f);
      const auto saved = recs::testing::read_file(f);
      fs::remove(f);
      CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
      recs::testing::write_file(f, saved);
    }
    CHECK_NOTHROW(recs::load_artifact(dir));
  }
  SECTION("an unlisted file at the top level") {
    recs::testing::write_file(dir / "extra.bin", "x");
    CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  }
  SECTION("an unlisted file inside a model directory") {
    recs::testing::write_file(dir / "pop" / "extra.bin", "x");
    CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  }
  SECTION("an empty extra file") {
    recs::testing::write_file(dir / "catalog" / "empty", "");
    CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  }
}

TEST_CASE("artifact: a truncated or malformed manifest fails verification", "[c1][artifact]") {
  TempDir root("art-manifest");
  const auto dir = write_sample(root.path());
  const auto manifest = recs::testing::read_file(dir / "manifest.json");
  for (std::size_t len : {std::size_t{0}, std::size_t{1}, manifest.size() / 2, manifest.size() - 3}) {
    INFO("len = " << len);
    recs::testing::write_file(dir / "manifest.json", manifest.substr(0, len));
    CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  }

  auto j = nlohmann::json::parse(manifest);
  auto edit = [&](auto fn) {
    auto k = j;
    fn(k);
    recs::testing::write_file(dir / "manifest.json", k.dump(2));
    CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  };
  edit([](nlohmann::json& k) { k["format_version"] = 2; });
  edit([](nlohmann::json& k) { k["default_model"] = "als"; });
  edit([](nlohmann::json& k) { k.erase("files"); });
  edit([](nlohmann::json& k) { k["files"]["../escape.bin"] = std::string(64, '0'); });
  edit([](nlohmann::json& k) { k["models"] = nlohmann::json::array({"pop"}); });  // pop_count/ unlisted
  edit([](nlohmann::json& k) { k["models"] = nlohmann::json::array({"pop", "nope"}); });

  recs::testing::write_file(dir / "manifest.json", manifest);
  CHECK_NOTHROW(recs::load_artifact(dir));
  fs::remove(dir / "manifest.json");
  CHECK_THROWS_AS(recs::load_artifact(dir), recs::ArtifactError);
  CHECK_THROWS_AS(recs::load_artifact(root.path() / "does-not-exist"), recs::ArtifactError);
}

TEST_CASE("artifact: the manifest version must match the directory name", "[c1][artifact]") {
  TempDir root("art-rename");
  const auto dir = write_sample(root.path());
  fs::rename(dir, root.path() / "2027-01-01T00-00-00Z");
  CHECK_THROWS_AS(recs::load_artifact(root.path() / "2027-01-01T00-00-00Z"), recs::ArtifactError);
}

TEST_CASE("artifact: an inconsistent catalog is rejected", "[c1][artifact][catalog]") {
  TempDir root("art-badcat");
  const Fitted f;
  auto cat = recs::make_catalog(fixture());
  cat.popularity_rank[0] = cat.popularity_rank[1];  // not a permutation of 1..n
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, f.list(), "pop", "x", "d"));
  cat = recs::make_catalog(fixture());
  cat.mean_rating[2] = std::nanf("");
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, f.list(), "pop", "x", "d"));
  cat = recs::make_catalog(fixture());
  cat.rating_count.pop_back();
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, f.list(), "pop", "x", "d"));
}

TEST_CASE("artifact: leftover .tmp-* directories are ignored and cleaned up", "[c1][artifact]") {
  TempDir root("art-tmp");
  const auto stale = root.path() / ".tmp-2026-01-01T00-00-00Z-1";
  fs::create_directories(stale / "pop");
  recs::testing::write_file(stale / "pop" / "half-written.bin", "partial");
  const auto dir = write_sample(root.path());
  CHECK_NOTHROW(recs::load_artifact(dir));
  CHECK_FALSE(fs::exists(stale));
}

TEST_CASE("artifact: invalid arguments are rejected", "[c1][artifact]") {
  TempDir root("art-args");
  const Fitted f;
  const auto cat = recs::make_catalog(fixture());
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, f.list(), "als", "x", "d"));
  CHECK_THROWS(recs::write_artifact(root.path(), "../v1", cat, f.list(), "pop", "x", "d"));
  CHECK_THROWS(recs::write_artifact(root.path(), ".tmp-v1", cat, f.list(), "pop", "x", "d"));
  CHECK_THROWS(recs::write_artifact(root.path(), "", cat, f.list(), "pop", "x", "d"));
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, {f.pop.get(), f.pop.get()}, "pop", "x", "d"));
  CHECK_THROWS(recs::write_artifact(root.path(), "v1", cat, {}, "pop", "x", "d"));
  recs::write_artifact(root.path(), "v1", cat, f.list(), "pop", "x", "d");
  CHECK_THROWS_AS(recs::write_artifact(root.path(), "v1", cat, f.list(), "pop", "x", "d"), recs::ArtifactError);
}

TEST_CASE("CURRENT: write/read round-trip, replaced via rename", "[c1][artifact][current]") {
  TempDir root("art-current");
  CHECK_FALSE(recs::read_current(root.path()).has_value());

  recs::write_current(root.path(), "2026-11-10T00-00-00Z");
  CHECK(recs::read_current(root.path()) == "2026-11-10T00-00-00Z");
  CHECK(recs::testing::read_file(root.path() / "CURRENT") == "2026-11-10T00-00-00Z\n");

  struct stat before {};
  REQUIRE(::stat((root.path() / "CURRENT").c_str(), &before) == 0);
  // Keep a second link to the old file: an in-place write would change its content too.
  fs::create_hard_link(root.path() / "CURRENT", root.path() / "old-link");

  recs::write_current(root.path(), "2026-11-11T00-00-00Z");
  CHECK(recs::read_current(root.path()) == "2026-11-11T00-00-00Z");
  struct stat after {};
  REQUIRE(::stat((root.path() / "CURRENT").c_str(), &after) == 0);
  CHECK(before.st_ino != after.st_ino);
  CHECK(recs::testing::read_file(root.path() / "old-link") == "2026-11-10T00-00-00Z\n");

  for (const auto& e : fs::directory_iterator(root.path())) {
    CHECK(e.path().filename().string().find(".tmp") == std::string::npos);
  }

  CHECK_THROWS(recs::write_current(root.path(), "../evil"));
  CHECK_THROWS(recs::write_current(root.path(), ""));
  recs::testing::write_file(root.path() / "CURRENT", "../evil\n");
  CHECK_THROWS_AS(recs::read_current(root.path()), recs::ArtifactError);
}
