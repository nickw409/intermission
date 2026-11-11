// Hot swap through the CURRENT watcher, end to end over the Unix socket, with real artifacts built
// from tests/fixtures/tiny/ (spec Phase 5: zero failed requests during a swap, exactly one
// X-Model-Version per response, a corrupted artifact is rejected and the old one keeps serving).
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "fixtures.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "server.hpp"
#include "service.hpp"
#include "watcher.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using Poll = recs::service::ArtifactWatcher::Poll;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("rw-" + std::to_string(::getpid()) + "-" + std::to_string(rd() % 100000));
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

// A pop + pop_count artifact; `m` varies the pop damping so versions score differently.
void publish(const fs::path& root, const std::string& version, double m, bool point_current = true) {
  auto pop = recs::make_popularity();
  pop->fit(tiny(), recs::Config{{"m", m}}, {});
  auto count = recs::make_popularity_count();
  count->fit(tiny(), recs::Config::object(), {});
  recs::write_artifact(root, version, recs::make_catalog(tiny()), {pop.get(), count.get()}, "pop", "test", "tiny");
  if (point_current) recs::write_current(root, version);
}

}  // namespace

TEST_CASE("recs-model: watcher: loads CURRENT, ignores unchanged, rejects corrupted and partial artifacts", "[model-service][swap]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  recs::service::ArtifactWatcher w(root, holder, metrics, svc);

  CHECK(w.poll_once() == Poll::no_current);
  CHECK(holder.get() == nullptr);

  publish(root, "v1", 10);
  CHECK(w.poll_once() == Poll::swapped);
  REQUIRE(holder.get() != nullptr);
  CHECK(holder.get()->version == "v1");
  CHECK(w.poll_once() == Poll::unchanged);

  // Corrupted: one byte appended to a model file after the manifest was written.
  publish(root, "v2", 5);
  {
    std::ofstream out(root / "v2" / "pop" / "scores.bin", std::ios::binary | std::ios::app);
    out << 'x';
  }
  CHECK(w.poll_once() == Poll::failed);
  CHECK(holder.get()->version == "v1");
  CHECK(w.poll_once() == Poll::unchanged);  // a rejected version is not re-verified every poll
  CHECK(svc.handle({"GET", "/v1/version", {}, "", ""}).model_version == "v1");
  CHECK(svc.handle({"GET", "/metrics", {}, "", ""}).body.find("recs_model_artifact_load_failures_total 1\n") != std::string::npos);

  // Partial: CURRENT names a directory without its manifest.
  publish(root, "v3", 5, false);
  fs::remove(root / "v3" / "manifest.json");
  recs::write_current(root, "v3");
  CHECK(w.poll_once() == Poll::failed);
  CHECK(holder.get()->version == "v1");

  // An unlisted extra file is rejected too.
  publish(root, "v4", 5, false);
  std::ofstream(root / "v4" / "pop" / "extra.bin") << "x";
  recs::write_current(root, "v4");
  CHECK(w.poll_once() == Poll::failed);
  CHECK(holder.get()->version == "v1");

  // A good artifact afterwards is picked up.
  publish(root, "v5", 5);
  CHECK(w.poll_once() == Poll::swapped);
  CHECK(holder.get()->version == "v5");

  // The background thread polls on its own.
  publish(root, "v6", 3);
  w.start(std::chrono::milliseconds(20));
  for (int i = 0; i < 500 && holder.get()->version != "v6"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  w.stop();
  CHECK(holder.get()->version == "v6");
}

TEST_CASE("recs-model: no artifact: 503 model_unavailable everywhere except /metrics; healthz 503", "[model-service][swap]") {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  const std::vector<recs::service::Request> reqs = {
      {"POST", "/v1/recommend", {}, "", R"({"model":"pop","ratings":[],"k":3})"},
      {"POST", "/v1/compare", {}, "", R"({"models":["pop"],"ratings":[],"k":3})"},
      {"GET", "/v1/similar/1", {}, "", ""},
      {"POST", "/v1/onboarding", {}, "", R"({"n":3})"},
      {"GET", "/v1/movies/search", {{"q", "a"}}, "", ""},
      {"POST", "/v1/movies/batch", {}, "", R"({"ids":[1]})"},
      {"GET", "/v1/version", {}, "", ""},
      {"GET", "/v1/models", {}, "", ""},
      // Even invalid input gets 503 first: nothing is served without a verified artifact.
      {"POST", "/v1/recommend", {}, "", "{nope"},
  };
  for (const auto& r : reqs) {
    INFO(r.path);
    const auto res = svc.handle(r);
    CHECK(res.status == 503);
    CHECK(res.model_version == "none");
    CHECK(json::parse(res.body) == json{{"error", {{"code", "model_unavailable"}, {"message", "no verified artifact is loaded"}}}});
  }
  CHECK(svc.handle({"GET", "/healthz", {}, "", ""}).status == 503);
  CHECK(svc.handle({"GET", "/metrics", {}, "", ""}).status == 200);
}

TEST_CASE("recs-model: hot swap under 8 concurrent clients: zero failed requests, one version header each", "[model-service][swap]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  publish(root, "v1", 10);

  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  recs::service::ArtifactWatcher watcher(root, holder, metrics, svc);
  REQUIRE(watcher.poll_once() == Poll::swapped);

  const fs::path sock = tmp.path / "m.sock";
  recs::service::HttpServer server(svc);
  std::string err;
  REQUIRE(server.bind({sock.string(), "", 0, 8, 0660}, &err));
  std::thread serve([&] { server.run(); });
  server.wait_until_ready();

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<int> served{0};
  std::mutex seen_mu;
  std::set<std::string> seen;
  const std::string body = R"({"model":"pop","ratings":[{"movie_id":1,"rating":4},{"movie_id":9,"rating":5}],"k":10})";
  std::vector<std::thread> clients;
  for (int t = 0; t < 8; ++t) {
    clients.emplace_back([&] {
      httplib::Client c(sock.string(), 80);
      c.set_address_family(AF_UNIX);
      c.set_read_timeout(10, 0);
      while (!stop.load()) {
        auto r = c.Post("/v1/recommend", body, "application/json");
        const bool ok = r && r->status == 200 && r->get_header_value_count("X-Model-Version") == 1 &&
                        json::parse(r->body)["items"].size() == 10;
        if (!ok) {
          if (failures.load() < 5) {
            std::lock_guard lock(seen_mu);
            if (!r) {
              UNSCOPED_INFO("transport error " << httplib::to_string(r.error()));
            } else {
              UNSCOPED_INFO("status " << r->status << " headers " << r->get_header_value_count("X-Model-Version") << " body " << r->body.substr(0, 200));
            }
          }
          ++failures;
        } else {
          std::lock_guard lock(seen_mu);
          seen.insert(r->get_header_value("X-Model-Version"));
        }
        ++served;
      }
    });
  }
  const auto wait_served = [&](int n) {
    const int target = served.load() + n;
    while (served.load() < target) std::this_thread::yield();
  };
  wait_served(50);
  publish(root, "v2", 5);
  CHECK(watcher.poll_once() == Poll::swapped);
  wait_served(50);
  publish(root, "v3", 2);
  CHECK(watcher.poll_once() == Poll::swapped);
  wait_served(50);
  stop = true;
  for (auto& c : clients) c.join();
  server.stop();
  serve.join();

  CHECK(failures.load() == 0);
  CHECK(served.load() >= 150);
  CHECK(seen.count("v3") == 1);
  for (const auto& v : seen) CHECK((v == "v1" || v == "v2" || v == "v3"));
}

TEST_CASE("recs-model: watcher rejects CURRENT escaping the root and symlinked version directories", "[model-service][swap]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  publish(root, "v1", 10);
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  recs::service::ArtifactWatcher w(root, holder, metrics, svc);
  REQUIRE(w.poll_once() == Poll::swapped);

  // An artifact outside the root that would verify on its own.
  const fs::path outside = tmp.path / "outside";
  publish(outside, "x", 3, false);
  for (const std::string& current : {std::string("../outside/x"), std::string("/abs"), (outside / "x").string(),
                                    std::string("."), std::string("..")}) {
    INFO(current);
    std::ofstream(root / "CURRENT", std::ios::trunc) << current << "\n";
    CHECK(w.poll_once() == Poll::failed);
    CHECK(holder.get()->version == "v1");
    CHECK_THROWS_AS(recs::service::load_current_bundle(root), recs::ArtifactError);
  }

  // A version directory that is a symlink to a valid artifact elsewhere.
  publish(outside, "v9", 3, false);
  fs::create_directory_symlink(outside / "v9", root / "v9");
  recs::write_current(root, "v9");
  CHECK(w.poll_once() == Poll::failed);
  CHECK(holder.get()->version == "v1");
  CHECK_THROWS_AS(recs::service::load_current_bundle(root), recs::ArtifactError);
}

TEST_CASE("recs-model: watcher logs an unreadable CURRENT once, not on every poll", "[model-service][swap]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  publish(root, "v1", 10);
  std::vector<std::string> lines;
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [&](std::string_view line) { lines.emplace_back(line); });
  recs::service::ArtifactWatcher w(root, holder, metrics, svc);
  const auto warnings = [&] {
    return std::count_if(lines.begin(), lines.end(),
                         [](const std::string& l) { return l.find("cannot read CURRENT") != std::string::npos; });
  };

  // A symlinked CURRENT (rejected since SA2-5): one warning however many polls see it.
  fs::rename(root / "CURRENT", root / "real-current");
  fs::create_symlink(root / "real-current", root / "CURRENT");
  for (int i = 0; i < 3; ++i) CHECK(w.poll_once() == Poll::failed);
  CHECK(warnings() == 1);

  // A different problem is reported again, once.
  fs::remove(root / "CURRENT");
  std::ofstream(root / "CURRENT") << std::string(1000, 'a');
  for (int i = 0; i < 3; ++i) CHECK(w.poll_once() == Poll::failed);
  CHECK(warnings() == 2);

  // Once CURRENT is readable again, a later failure is reported again.
  recs::write_current(root, "v1");
  CHECK(w.poll_once() == Poll::swapped);
  fs::remove(root / "CURRENT");
  fs::create_symlink(root / "real-current", root / "CURRENT");
  CHECK(w.poll_once() == Poll::failed);
  CHECK(w.poll_once() == Poll::failed);
  CHECK(warnings() == 3);
}
