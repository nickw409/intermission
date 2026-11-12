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

#include "check.hpp"
#include "fixtures.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

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
  CHECK(r.code == 0);
  CHECK(r.out == "version=2026-11-12T00-00-00Z\ngit_sha=abc1234\nmodels=pop,pop_count\n");
}

TEST_CASE("check: a git_sha that is not a hex commit id is reported as unknown", "[check]") {
  // The value is printed for a shell script to read: never pass through arbitrary manifest text.
  for (const std::string sha : {"test", "abc12", "abc1234\nversion=x", "ABC1234", ""}) {
    TempDir t;
    publish(t.path, "2026-11-12T00-00-00Z", sha);
    const auto r = run_check(t.path);
    CHECK(r.code == 0);
    CHECK(contains(r.out, "\ngit_sha=unknown\n"));
  }
}

TEST_CASE("check: no CURRENT pointer fails", "[check]") {
  TempDir t;
  const auto r = run_check(t.path);
  CHECK(r.code == 1);
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
  CHECK(r.code == 1);
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
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(contains(r.log, "artifact 2026-11-12T01-00-00Z rejected"));
  CHECK(contains(r.log, "does not match"));
}
