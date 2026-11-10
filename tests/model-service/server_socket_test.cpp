// End to end over a real Unix socket: recs-model serving a popularity-only artifact built from
// tests/fixtures/tiny/ with the real librecs pipeline (load_movielens -> fit -> write_artifact ->
// CURRENT -> load_current_bundle), as plan §1.3 describes.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "bundle.hpp"
#include "fixtures.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "schema.hpp"
#include "server.hpp"
#include "service.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    // Unix socket paths are limited to ~108 bytes, so keep this short.
    path = fs::temp_directory_path() / ("rm-" + std::to_string(::getpid()) + "-" + std::to_string(rd() % 100000));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

const char* kVersion = "2026-11-10T00-00-00Z";

void build_tiny_artifact(const fs::path& root) {
  const recs::Dataset ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  auto pop = recs::make_popularity();
  pop->fit(ds, recs::Config{{"m", 10}}, {});
  auto count = recs::make_popularity_count();
  count->fit(ds, recs::Config::object(), {});
  recs::write_artifact(root, kVersion, recs::make_catalog(ds), {pop.get(), count.get()}, "pop", "test", "tiny");
  recs::write_current(root, kVersion);
}

struct RunningServer {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc{holder, metrics, [](std::string_view) {}};
  recs::service::HttpServer server{svc};
  std::thread thread;

  explicit RunningServer(const recs::service::ListenOptions& opts) {
    std::string err;
    REQUIRE(server.bind(opts, &err));
    thread = std::thread([this] { server.run(); });
    server.wait_until_ready();
  }
  ~RunningServer() {
    server.stop();
    thread.join();
  }
};

httplib::Client unix_client(const fs::path& sock) {
  httplib::Client c(sock.string(), 80);
  c.set_address_family(AF_UNIX);
  c.set_read_timeout(5, 0);
  return c;
}

}  // namespace

TEST_CASE("recs-model serves the tiny popularity artifact over a Unix socket", "[model-service][server]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  build_tiny_artifact(root);
  const fs::path sock = tmp.path / "model.sock";

  RunningServer rs({sock.string(), "", 0, 4, 0660});
  // Mode: owner and group read/write only (the recs group), nobody else.
  struct stat st {};
  REQUIRE(::stat(sock.c_str(), &st) == 0);
  CHECK(S_ISSOCK(st.st_mode));
  CHECK((st.st_mode & 0777) == 0660);

  auto cli = unix_client(sock);

  SECTION("503 model_unavailable until an artifact is loaded") {
    auto r = cli.Get("/v1/version");
    REQUIRE(r);
    CHECK(r->status == 503);
    CHECK(r->get_header_value("X-Model-Version") == "none");
    CHECK(json::parse(r->body)["error"]["code"] == "model_unavailable");
    auto h = cli.Get("/healthz");
    REQUIRE(h);
    CHECK(h->status == 503);
    CHECK(json::parse(h->body) == json{{"status", "unavailable"}});
  }

  SECTION("every route once the tiny artifact is loaded") {
    rs.holder.store(recs::service::load_current_bundle(root));

    httplib::Headers hdrs = {{"X-Request-ID", "tiny-trace-1"}};
    auto v = cli.Get("/v1/version", hdrs);
    REQUIRE(v);
    CHECK(v->status == 200);
    CHECK(v->get_header_value("X-Model-Version") == kVersion);
    CHECK(v->get_header_value("X-Request-ID") == "tiny-trace-1");
    CHECK(json::parse(v->body) == json{{"artifact_version", kVersion}});

    auto models = cli.Get("/v1/models");
    REQUIRE(models);
    const json mb = json::parse(models->body);
    CHECK(recs::testing::validate_against(mb, "ModelsResponse").empty());
    CHECK(mb["models"] == json{"pop", "pop_count"});
    CHECK(mb["default_model"] == "pop");

    // Toy Story (1) and Star Wars (9) rated; recommendations exclude them.
    const json req = {{"model", "pop"}, {"ratings", {{{"movie_id", 1}, {"rating", 4}}, {{"movie_id", 9}, {"rating", 5}}, {{"movie_id", 424242}, {"rating", 3}}}}, {"k", 5}};
    auto rec = cli.Post("/v1/recommend", req.dump(), "application/json");
    REQUIRE(rec);
    CHECK(rec->status == 200);
    const json rb = json::parse(rec->body);
    CHECK(recs::testing::validate_against(rb, "RecommendResponse").empty());
    CHECK(rb["ignored"] == 1);
    CHECK(rb["items"].size() == 5);
    for (const auto& it : rb["items"]) {
      CHECK(it["movie_id"] != 1);
      CHECK(it["movie_id"] != 9);
    }

    const json creq = {{"models", {"pop_count", "pop"}}, {"ratings", json::array()}, {"k", 3}};
    auto cmp = cli.Post("/v1/compare", creq.dump(), "application/json");
    REQUIRE(cmp);
    CHECK(recs::testing::validate_against(json::parse(cmp->body), "CompareResponse").empty());

    auto knn = cli.Get("/v1/similar/9?model=knn&k=3");
    REQUIRE(knn);
    CHECK(knn->status == 400);
    CHECK(json::parse(knn->body)["error"]["code"] == "unknown_model");

    auto search = cli.Get("/v1/movies/search?q=amelie&limit=5");
    REQUIRE(search);
    CHECK(json::parse(search->body) == json{{"movie_ids", {18}}});

    auto onb = cli.Post("/v1/onboarding", R"({"n":5,"exclude":[1]})", "application/json");
    REQUIRE(onb);
    CHECK(json::parse(onb->body)["movie_ids"].size() == 5);

    auto batch = cli.Post("/v1/movies/batch", R"({"ids":[27,30,999]})", "application/json");
    REQUIRE(batch);
    const json bb = json::parse(batch->body);
    CHECK(recs::testing::validate_against(bb, "BatchResponse").empty());
    CHECK(bb["missing"] == json{999});
    CHECK(bb["movies"][0]["year"].is_null());        // "Hamlet, Prince of Denmark" has no year
    CHECK(bb["movies"][1]["rating_count"] == 0);     // "Never Rated"
    CHECK(bb["movies"][1]["mean_rating"].is_null());

    auto health = cli.Get("/healthz");
    REQUIRE(health);
    CHECK(health->status == 200);

    auto metrics = cli.Get("/metrics");
    REQUIRE(metrics);
    CHECK(metrics->status == 200);
    CHECK(metrics->body.find("recs_model_requests_total{route=\"/v1/version\",status=\"200\"} 1") != std::string::npos);

    // Over 1 MiB: rejected by the HTTP layer, answered in the contract's Error shape.
    const std::string big = "{\"ids\":[" + std::string((1u << 20) + 10, ' ') + "1]}";
    auto tl = cli.Post("/v1/movies/batch", big, "application/json");
    REQUIRE(tl);
    CHECK(tl->status == 400);
    CHECK(tl->get_header_value("X-Model-Version") == kVersion);
    CHECK(json::parse(tl->body)["error"]["code"] == "body_too_large");
    // Exactly one X-Model-Version on service responses, error responses and httplib-generated ones.
    CHECK(v->get_header_value_count("X-Model-Version") == 1);
    CHECK(knn->get_header_value_count("X-Model-Version") == 1);
    CHECK(tl->get_header_value_count("X-Model-Version") == 1);
    auto unknown = cli.Get("/v1/nope");
    REQUIRE(unknown);
    CHECK(unknown->status == 404);
    CHECK(unknown->get_header_value_count("X-Model-Version") == 1);
    CHECK(json::parse(unknown->body)["error"]["code"] == "not_found");

    auto bad = cli.Post("/v1/recommend", "{not json", "application/json");
    REQUIRE(bad);
    CHECK(bad->status == 400);
    CHECK(json::parse(bad->body)["error"]["code"] == "invalid_json");
  }
}

TEST_CASE("load_current_bundle rejects a corrupted artifact", "[model-service][server]") {
  TempDir tmp;
  const fs::path root = tmp.path / "artifacts";
  build_tiny_artifact(root);
  CHECK(recs::service::load_current_bundle(root)->version == kVersion);
  {
    std::ofstream out(root / kVersion / "pop" / "scores.bin", std::ios::binary | std::ios::app);
    out << "x";
  }
  CHECK_THROWS_AS(recs::service::load_current_bundle(root), recs::ArtifactError);
  CHECK_THROWS_AS(recs::service::load_current_bundle(tmp.path / "nothing-here"), recs::ArtifactError);
}

TEST_CASE("TCP fallback binds loopback only; stale sockets are replaced, other files are not", "[model-service][server]") {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  {
    recs::service::HttpServer s(svc);
    std::string err;
    CHECK_FALSE(s.bind({"", "0.0.0.0", 0, 2, 0660}, &err));
    CHECK(err.find("loopback") != std::string::npos);
  }
  {
    RunningServer rs({"", "127.0.0.1", 0, 2, 0660});
    httplib::Client c("127.0.0.1", rs.server.port());
    auto r = c.Get("/healthz");
    REQUIRE(r);
    CHECK(r->status == 503);
  }
  TempDir tmp;
  const fs::path sock = tmp.path / "m.sock";
  {
    // A socket file left behind by a crashed process (bound, then closed without unlink).
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    REQUIRE(fs::is_socket(sock));
  }
  {
    RunningServer rs({sock.string(), "", 0, 2, 0660});  // the leftover socket file is replaced
    auto c = unix_client(sock);
    auto r = c.Get("/healthz");
    REQUIRE(r);
    CHECK(r->status == 503);
  }
  CHECK_FALSE(fs::exists(sock));  // removed on shutdown
  const fs::path regular = tmp.path / "not-a-socket";
  std::ofstream(regular) << "keep me";
  recs::service::HttpServer s(svc);
  std::string err;
  CHECK_FALSE(s.bind({regular.string(), "", 0, 2, 0660}, &err));
  CHECK(fs::file_size(regular) == 7);
  // sockaddr_un holds at most 107 path bytes; a longer path is refused with a clear message.
  recs::service::HttpServer s2(svc);
  CHECK_FALSE(s2.bind({(tmp.path / std::string(120, 's')).string(), "", 0, 2, 0660}, &err));
  CHECK(err.find("longer than") != std::string::npos);
}
