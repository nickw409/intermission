// Cross-cutting service behaviour: X-Model-Version and X-Request-ID on every response, logs that
// never contain rating payloads, /metrics, and bundle snapshots across a swap.
#include <atomic>
#include <stdexcept>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "service.hpp"
#include "test_bundle.hpp"

namespace {

using nlohmann::json;
using recs::service::Request;

struct Captured {
  std::mutex mu;
  std::vector<std::string> lines;
  recs::service::LogSink sink() {
    return [this](std::string_view l) {
      std::lock_guard lock(mu);
      lines.emplace_back(l);
    };
  }
};

}  // namespace

TEST_CASE("recs-model: every response carries X-Model-Version and X-Request-ID", "[model-service][headers]") {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  const std::vector<Request> reqs = {
      {"GET", "/healthz", {}, "", ""},          {"GET", "/metrics", {}, "", ""},
      {"GET", "/v1/version", {}, "", ""},       {"GET", "/nope", {}, "", ""},
      {"POST", "/v1/recommend", {}, "", "{x"},  {"PUT", "/v1/models", {}, "", ""},
  };
  for (const char* expected : {"none", "2026-11-09T21-14-03Z"}) {
    for (const auto& r : reqs) {
      const auto res = svc.handle(r);
      INFO(r.method << " " << r.path);
      CHECK(res.model_version == expected);
      CHECK(res.request_id.size() == 32);  // generated: 16 random bytes in hex
    }
    holder.store(recs::testing::fixture_bundle());
  }
  CHECK(svc.transport_error(413, "abc").model_version == "2026-11-09T21-14-03Z");
  CHECK(json::parse(svc.transport_error(413, "abc").body)["error"]["code"] == "body_too_large");
  CHECK(svc.transport_error(413, "abc").status == 400);
  CHECK(svc.transport_error(413, "abc").request_id == "abc");

  // A valid caller id is echoed; an invalid one (too long, bad characters) is replaced.
  CHECK(svc.handle({"GET", "/v1/version", {}, "req-1.A_b", ""}).request_id == "req-1.A_b");
  const auto bad1 = svc.handle({"GET", "/v1/version", {}, "a\r\nX-Evil: 1", ""}).request_id;
  CHECK(bad1.size() == 32);
  CHECK(svc.handle({"GET", "/v1/version", {}, std::string(129, 'a'), ""}).request_id.size() == 32);
  CHECK(svc.handle({"GET", "/v1/version", {}, std::string(128, 'a'), ""}).request_id == std::string(128, 'a'));
}

TEST_CASE("recs-model: logs never contain rating payloads", "[model-service][logging]") {
  recs::service::BundleHolder holder;
  holder.store(recs::testing::fixture_bundle());
  recs::service::Metrics metrics;
  Captured cap;
  recs::service::Service svc(holder, metrics, cap.sink());

  // Distinctive values that must not appear anywhere in the logs.
  const json body = {{"model", "knn"},
                     {"ratings", {{{"movie_id", 4973}, {"rating", 4.5}}, {{"movie_id", 1234567}, {"rating", 0.5}}}},
                     {"k", 7},
                     {"exclude", {7003}}};
  svc.handle({"POST", "/v1/recommend", {}, "trace-42", body.dump()});
  json cmp = body;
  cmp.erase("model");
  cmp["models"] = {"pop", "knn"};
  svc.handle({"POST", "/v1/compare", {}, "trace-43", cmp.dump()});
  json bad = body;
  bad["ratings"][0]["rating"] = 4.3;
  svc.handle({"POST", "/v1/recommend", {}, "trace-44", bad.dump()});
  svc.handle({"POST", "/v1/recommend", {}, "trace-45", "{\"ratings\":[{\"movie_id\":4973,\"rating\":4.5}"});
  svc.handle({"GET", "/v1/movies/search", {{"q", "secret-query-text"}}, "trace-46", ""});

  REQUIRE(cap.lines.size() == 5);
  for (const auto& line : cap.lines) {
    INFO(line);
    const json j = json::parse(line);  // each line is one JSON object
    // Exactly these fields: nothing derived from the body or the query string.
    std::set<std::string> keys;
    for (const auto& [k, v] : j.items()) keys.insert(k);
    CHECK(keys == std::set<std::string>{"ts", "level", "msg", "request_id", "method", "route", "status",
                                        "latency_ms", "model_version"});
    CHECK(j["latency_ms"].is_number());
    CHECK(j["status"].is_number_integer());
    for (const auto& [k, v] : j.items()) {
      if (!v.is_string() || k == "ts") continue;  // the timestamp is ours, not the caller's
      for (const char* forbidden : {"4973", "1234567", "4.5", "0.5", "7003", "rating", "secret", "4.3"}) {
        CHECK(v.get<std::string>().find(forbidden) == std::string::npos);
      }
    }
    CHECK(line.find("movie_id") == std::string::npos);
    CHECK(line.find("rating") == std::string::npos);
    CHECK(line.find("secret-query-text") == std::string::npos);
  }
  CHECK(json::parse(cap.lines[0])["request_id"] == "trace-42");
  CHECK(json::parse(cap.lines[0])["route"] == "/v1/recommend");
  CHECK(json::parse(cap.lines[2])["status"] == 400);
}

TEST_CASE("recs-model: /metrics exposes request, latency, fold-in and model version series", "[model-service][metrics]") {
  recs::service::BundleHolder holder;
  holder.store(recs::testing::fixture_bundle());
  recs::service::Metrics metrics;
  metrics.set_artifact_load_seconds(0.25);
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  svc.handle({"POST", "/v1/recommend", {}, "", R"({"model":"pop","ratings":[],"k":3})"});
  svc.handle({"GET", "/v1/similar/260", {}, "", ""});
  svc.handle({"GET", "/v1/similar/999999", {}, "", ""});
  svc.handle({"GET", "/whatever/../../etc/passwd", {}, "", ""});
  const auto res = svc.handle({"GET", "/metrics", {}, "", ""});
  REQUIRE(res.status == 200);
  CHECK(res.content_type.starts_with("text/plain"));
  const std::string& m = res.body;
  INFO(m);
  CHECK(m.find("recs_model_requests_total{route=\"/v1/recommend\",status=\"200\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_requests_total{route=\"/v1/similar/{movie_id}\",status=\"200\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_requests_total{route=\"/v1/similar/{movie_id}\",status=\"404\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_requests_total{route=\"other\",status=\"404\"} 1\n") != std::string::npos);
  CHECK(m.find("etc/passwd") == std::string::npos);  // raw paths never become labels
  CHECK(m.find("recs_model_request_duration_seconds_bucket{route=\"/v1/recommend\",le=\"+Inf\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_request_duration_seconds_count{route=\"/v1/recommend\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_foldin_duration_seconds_count{model=\"pop\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_artifact_info{version=\"2026-11-09T21-14-03Z\"} 1\n") != std::string::npos);
  CHECK(m.find("recs_model_artifact_load_seconds 0.25\n") != std::string::npos);
  CHECK(m.find("recs_model_artifact_loaded 1\n") != std::string::npos);
}

TEST_CASE("recs-model: a swap mid-traffic never mixes versions inside one response", "[model-service][swap]") {
  recs::service::BundleHolder holder;
  const auto v1 = recs::testing::fixture_bundle(std::vector<std::string>{"pop", "pop_count", "knn", "content", "mf", "als"}, "v1");
  const auto v2 = recs::testing::fixture_bundle(std::vector<std::string>{"pop", "knn"}, "v2");
  holder.store(v1);
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<int> served{0};
  std::vector<std::thread> clients;
  for (int t = 0; t < 4; ++t) {
    clients.emplace_back([&] {
      while (!stop.load()) {
        const auto r = svc.handle({"GET", "/v1/models", {}, "", ""});
        const json b = json::parse(r.body);
        // The header and the body come from the same snapshot.
        if (r.status != 200 || b["artifact_version"] != r.model_version ||
            (r.model_version != "v1" && r.model_version != "v2")) {
          ++failures;
        }
        ++served;
      }
    });
  }
  for (int i = 0; i < 200; ++i) holder.store(i % 2 ? v1 : v2);
  while (served.load() < 400) std::this_thread::yield();
  stop = true;
  for (auto& c : clients) c.join();
  CHECK(failures.load() == 0);
}

namespace {

// A model whose fold_in always throws, as a bug in model code would.
class ThrowingModel final : public recs::Model {
 public:
  explicit ThrowingModel(std::uint32_t n) : n_(n) {}
  std::string_view name() const override { return "als"; }
  void fit(const recs::Dataset&, const recs::Config&, const recs::TrainContext&) override {}
  recs::UserState fold_in(std::span<const recs::Rating>) const override { throw std::runtime_error("factor solve failed"); }
  void score(const recs::UserState&, std::span<float>) const override {}
  recs::Explanation explain(const recs::UserState&, recs::ItemIdx) const override { return {}; }
  void save(const recs::fs::path&) const override {}
  void load(const recs::fs::path&) override {}
  std::uint32_t n_items() const override { return n_; }
  recs::Config config() const override { return recs::Config::object(); }

 private:
  std::uint32_t n_;
};

}  // namespace

TEST_CASE("recs-model: a model exception is a 500 internal with exactly one error log line", "[model-service][logging]") {
  auto artifact = recs::testing::fixture_artifact();
  artifact.models["als"] = std::make_unique<ThrowingModel>(artifact.catalog.items.size());
  recs::service::BundleHolder holder;
  holder.store(recs::service::make_bundle(std::move(artifact)));
  recs::service::Metrics metrics;
  Captured cap;
  recs::service::Service svc(holder, metrics, cap.sink());

  const auto r = svc.handle({"POST", "/v1/recommend", {}, "boom-1", R"({"model":"als","ratings":[{"movie_id":260,"rating":5}],"k":3})"});
  CHECK(r.status == 500);
  CHECK(json::parse(r.body) == json{{"error", {{"code", "internal"}, {"message", "internal error"}}}});
  CHECK(r.model_version == recs::testing::kFixtureVersion);
  REQUIRE(cap.lines.size() == 1);
  const json line = json::parse(cap.lines[0]);
  CHECK(line["level"] == "error");
  CHECK(line["status"] == 500);
  CHECK(line["request_id"] == "boom-1");
  CHECK(line["error"] == "factor solve failed");
  CHECK(cap.lines[0].find("260") == std::string::npos);  // the payload is still not logged
}
