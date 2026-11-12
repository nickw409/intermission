// recs-model --check (check.hpp): the deploy preflight that asks a new recs-model whether it would
// serve the artifact CURRENT names. Artifacts are real ones built from tests/fixtures/tiny/.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "check.hpp"
#include "fixtures.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"
#include "recs/sha256.hpp"

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("rc-" + std::to_string(::getpid()) + "-" + std::to_string(rd() % 100000));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

const recs::Dataset& tiny() {
  static const recs::Dataset ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  return ds;
}

// A pop + pop_count artifact trained by commit `git_sha`; CURRENT points at it.
void publish(const fs::path& root, const std::string& version, const std::string& git_sha) {
  auto pop = recs::make_popularity();
  pop->fit(tiny(), recs::Config{{"m", 5.0}}, {});
  auto count = recs::make_popularity_count();
  count->fit(tiny(), recs::Config::object(), {});
  recs::write_artifact(root, version, recs::make_catalog(tiny()), {pop.get(), count.get()}, "pop", git_sha, "tiny");
  recs::write_current(root, version);
}

struct Result {
  int code;
  std::string out;
  std::string log;
};

Result run_check(const fs::path& root) {
  std::ostringstream out;
  std::string log;
  const int code = recs::service::check_artifacts(root, out, [&](std::string_view line) {
    log.append(line);
    log.push_back('\n');
  });
  return {code, out.str(), log};
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("check: a servable artifact passes and is described for the deploy script", "[check]") {
  TempDir t;
  publish(t.path, "2026-11-12T00-00-00Z", "abc1234");
  const auto r = run_check(t.path);
  CHECK(r.code == recs::service::kCheckOk);
  CHECK(r.code == 0);  // the deploy script relies on the literal values
  CHECK(r.out == "version=2026-11-12T00-00-00Z\ngit_sha=abc1234\nmodels=pop,pop_count\n");
}

TEST_CASE("check: a git_sha that is not a hex commit id is reported as unknown", "[check]") {
  // The value is printed for a shell script to read: never pass through arbitrary manifest text.
  for (const std::string& sha : std::vector<std::string>{"test", "abc12", "abc123", "abc1234\nversion=x", "ABC1234", "", "zzzzzzz", "release-1",
                                std::string(41, 'a'), "abc-1234"}) {
    INFO("git_sha " << sha);
    TempDir t;
    publish(t.path, "2026-11-12T00-00-00Z", sha);
    const auto r = run_check(t.path);
    CHECK(r.code == 0);
    CHECK(contains(r.out, "\ngit_sha=unknown\n"));
  }
}

TEST_CASE("check: short and full hex commit ids pass through unchanged", "[check]") {
  for (const std::string& sha : std::vector<std::string>{"abc1234", std::string(40, 'f'), "0123456789abcdef0123456789abcdef01234567"}) {
    INFO("git_sha " << sha);
    TempDir t;
    publish(t.path, "2026-11-12T00-00-00Z", sha);
    const auto r = run_check(t.path);
    CHECK(r.code == 0);
    CHECK(contains(r.out, "\ngit_sha=" + sha + "\n"));
  }
}

TEST_CASE("check: no CURRENT pointer fails", "[check]") {
  TempDir t;
  const auto r = run_check(t.path);
  CHECK(r.code == 4);
  CHECK(r.out.empty());
  CHECK(contains(r.log, "no CURRENT pointer"));
}

TEST_CASE("check: an artifact that fails verification fails", "[check]") {
  TempDir t;
  publish(t.path, "2026-11-12T00-00-00Z", "abc1234");
  // Flip one byte of a model file: its SHA-256 no longer matches the manifest.
  fs::path victim;
  for (const auto& e : fs::recursive_directory_iterator(t.path / "2026-11-12T00-00-00Z")) {
    if (e.is_regular_file() && e.path().filename() != "manifest.json") {
      victim = e.path();
      break;
    }
  }
  REQUIRE(!victim.empty());
  {
    std::fstream f(victim, std::ios::in | std::ios::out | std::ios::binary);
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x5a);
    f.seekp(0);
    f.write(&c, 1);
  }
  const auto r = run_check(t.path);
  CHECK(r.code == 3);
  CHECK(r.out.empty());
  CHECK(contains(r.log, "rejected"));
}

TEST_CASE("check: an intact artifact under another version's name fails", "[check]") {
  // Every file still matches its hash; only the manifest's version disagrees with CURRENT.
  TempDir t;
  publish(t.path, "2026-11-12T00-00-00Z", "abc1234");
  fs::copy(t.path / "2026-11-12T00-00-00Z", t.path / "2026-11-12T01-00-00Z", fs::copy_options::recursive);
  recs::write_current(t.path, "2026-11-12T01-00-00Z");
  const auto r = run_check(t.path);
  CHECK(r.code == 3);
  CHECK(r.out.empty());
  CHECK(contains(r.log, "artifact 2026-11-12T01-00-00Z rejected"));
  CHECK(contains(r.log, "does not match"));
}

TEST_CASE("check: a model file in a format this recs-model does not read fails", "[check]") {
  // The case the deploy preflight exists for (e.g. MF format 1 after min_support): every file
  // matches its manifest hash, but one model file carries a format version this binary refuses.
  TempDir t;
  const std::string version = "2026-11-12T00-00-00Z";
  publish(t.path, version, "abc1234");
  const fs::path dir = t.path / version;
  nlohmann::json manifest;
  {
    std::ifstream in(dir / "manifest.json");
    manifest = nlohmann::json::parse(in);
  }
  std::string key;
  for (const auto& [file, hash] : manifest.at("files").items()) {
    const fs::path f(file);
    if (f.filename() == "scores.bin" && f.parent_path().filename() == "pop") key = file;
  }
  REQUIRE(!key.empty());
  {
    // scores.bin: magic "RECSPOP1", then the u32 format version (little-endian) at offset 8.
    std::fstream f(dir / key, std::ios::in | std::ios::out | std::ios::binary);
    char magic[8] = {};
    f.read(magic, 8);
    REQUIRE(std::string(magic, 8) == "RECSPOP1");
    const char future[4] = {99, 0, 0, 0};
    f.seekp(8);
    f.write(future, 4);
  }
  manifest["files"][key] = recs::sha256_file_hex(dir / key);
  {
    std::ofstream out(dir / "manifest.json", std::ios::trunc);
    out << manifest.dump(2);
  }
  const auto r = run_check(t.path);
  CHECK(r.code == 3);
  CHECK(r.out.empty());
  CHECK(contains(r.log, "artifact " + version + " rejected"));
  CHECK(contains(r.log, "unsupported version"));
}
