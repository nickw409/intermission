// Input revalidation (contracts/model.yaml "Rules"): every limit, type and shape rule returns a
// 400 with the Error shape, the boundary values are accepted, and hostile bodies never crash.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "schema.hpp"
#include "service.hpp"
#include "test_bundle.hpp"

namespace {

using nlohmann::json;
using recs::service::Request;
using recs::service::Response;

struct Fixture {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc{holder, metrics, [](std::string_view) {}};
  Fixture() { holder.store(recs::testing::fixture_bundle()); }

  Response post(const std::string& path, const std::string& body) { return svc.handle({"POST", path, {}, "", body}); }
  Response post(const std::string& path, const json& body) { return post(path, body.dump()); }
  Response get(const std::string& path, std::vector<std::pair<std::string, std::string>> q = {}) {
    return svc.handle({"GET", path, std::move(q), "", ""});
  }
};

std::string code_of(const Response& r) {
  const json b = json::parse(r.body);
  INFO(r.body);
  REQUIRE(recs::testing::validate_against(b, "Error").empty());
  return b["error"]["code"];
}

json ratings(std::size_t n, std::int64_t first_id = 1000000) {
  json a = json::array();
  for (std::size_t i = 0; i < n; ++i) a.push_back({{"movie_id", first_id + static_cast<std::int64_t>(i)}, {"rating", 3.5}});
  return a;
}

json rec(json r, json k = 10) { return {{"model", "pop"}, {"ratings", std::move(r)}, {"k", std::move(k)}}; }

}  // namespace

TEST_CASE("recommend: ratings count limit is 5,000", "[model-service][validation]") {
  Fixture f;
  auto ok = f.post("/v1/recommend", rec(ratings(5000)));
  CHECK(ok.status == 200);
  CHECK(json::parse(ok.body)["ignored"] == 5000);  // all unknown ids: skipped and counted
  auto bad = f.post("/v1/recommend", rec(ratings(5001)));
  CHECK(bad.status == 400);
  CHECK(code_of(bad) == "bad_request");
}

TEST_CASE("recommend: k must be an integer in [1, 100]", "[model-service][validation]") {
  Fixture f;
  CHECK(f.post("/v1/recommend", rec(json::array(), 1)).status == 200);
  CHECK(f.post("/v1/recommend", rec(json::array(), 100)).status == 200);
  CHECK(f.post("/v1/recommend", rec(json::array(), 3.0)).status == 200);  // JSON Schema: 3.0 is an integer
  for (const json& k : {json(0), json(101), json(-1), json(3.5), json("3"), json(nullptr), json(true),
                        json(1e300), json(18446744073709551615ull), json::array()}) {
    INFO(k.dump());
    const auto r = f.post("/v1/recommend", rec(json::array(), k));
    CHECK(r.status == 400);
    CHECK(code_of(r) == "bad_request");
  }
  json missing_k = {{"model", "pop"}, {"ratings", json::array()}};
  CHECK(f.post("/v1/recommend", missing_k).status == 400);
}

TEST_CASE("recommend: ratings must be in [0.5, 5] in 0.5 steps", "[model-service][validation]") {
  Fixture f;
  for (double v : {0.5, 1.0, 2.5, 5.0}) {
    CHECK(f.post("/v1/recommend", rec(json::array({{{"movie_id", 260}, {"rating", v}}}))).status == 200);
  }
  CHECK(f.post("/v1/recommend", rec(json::array({{{"movie_id", 260}, {"rating", 4}}}))).status == 200);
  for (const json& v : {json(0), json(0.0), json(5.5), json(4.3), json(-0.5), json(0.25), json(1e308), json("4"),
                        json(nullptr), json(false)}) {
    INFO(v.dump());
    const auto r = f.post("/v1/recommend", rec(json::array({{{"movie_id", 260}, {"rating", v}}})));
    CHECK(r.status == 400);
    CHECK(code_of(r) == "bad_request");
  }
}

TEST_CASE("recommend: movie ids must be integers in [1, 2^31-1]", "[model-service][validation]") {
  Fixture f;
  CHECK(f.post("/v1/recommend", rec(json::array({{{"movie_id", 2147483647}, {"rating", 3}}}))).status == 200);
  for (const json& id : {json(0), json(-1), json(2147483648LL), json(1.5), json("260"), json(nullptr),
                         json(-9223372036854775807LL), json(18446744073709551615ull)}) {
    INFO(id.dump());
    CHECK(f.post("/v1/recommend", rec(json::array({{{"movie_id", id}, {"rating", 3}}}))).status == 400);
    json body = rec(json::array());
    body["exclude"] = json::array({id});
    CHECK(f.post("/v1/recommend", body).status == 400);
  }
}

TEST_CASE("recommend: object shapes are strict", "[model-service][validation]") {
  Fixture f;
  json extra = rec(json::array());
  extra["user"] = "alice";
  CHECK(code_of(f.post("/v1/recommend", extra)) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", rec(json::array({{{"movie_id", 1}}})))) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", rec(json::array({{{"movie_id", 1}, {"rating", 3}, {"x", 1}}})))) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", rec(json::array({1, 2})))) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", rec(json::object()))) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", json::array())) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", json(42))) == "bad_request");
  CHECK(code_of(f.post("/v1/recommend", std::string(""))) == "invalid_json");
  CHECK(code_of(f.post("/v1/recommend", std::string("{\"model\":\"pop\""))) == "invalid_json");
  CHECK(code_of(f.post("/v1/recommend", std::string("{\"model\":\"p\xff\"}"))) == "invalid_json");
  json model_not_string = rec(json::array());
  model_not_string["model"] = 7;
  CHECK(code_of(f.post("/v1/recommend", model_not_string)) == "bad_request");
}

TEST_CASE("recommend: duplicates, unknown ids, exclude", "[model-service][validation]") {
  Fixture f;
  // A duplicate movie_id is rejected even when the id is unknown to the catalog.
  auto dup = f.post("/v1/recommend", rec(json::array({{{"movie_id", 999999}, {"rating", 3}}, {{"movie_id", 999999}, {"rating", 4}}})));
  CHECK(dup.status == 400);
  CHECK(json::parse(dup.body)["error"]["message"] == "duplicate movie_id 999999 in ratings");

  json body = rec(json::array({{{"movie_id", 260}, {"rating", 5}}, {{"movie_id", 424242}, {"rating", 3}}}), 100);
  body["exclude"] = json::array({356, 5555555});
  auto r = f.post("/v1/recommend", body);
  REQUIRE(r.status == 200);
  const json b = json::parse(r.body);
  CHECK(b["ignored"] == 1);
  for (const auto& it : b["items"]) {
    CHECK(it["movie_id"] != 260);
    CHECK(it["movie_id"] != 356);
  }
  CHECK(b["items"].size() == recs::testing::fixture_catalog_rows().size() - 2);

  json big = rec(json::array());
  big["exclude"] = json::array();
  for (int i = 1; i <= 10000; ++i) big["exclude"].push_back(i);
  CHECK(f.post("/v1/recommend", big).status == 200);
  big["exclude"].push_back(10001);
  CHECK(code_of(f.post("/v1/recommend", big)) == "bad_request");
}

TEST_CASE("model names: unknown, reserved and not loaded", "[model-service][validation]") {
  Fixture f;
  for (const char* m : {"svd", "hybrid", "mmr", "", "POP"}) {
    json body = rec(json::array());
    body["model"] = m;
    const auto r = f.post("/v1/recommend", body);
    CHECK(r.status == 400);
    CHECK(code_of(r) == "unknown_model");
  }
  json huge = rec(json::array());
  huge["model"] = std::string(100000, 'x');
  const auto r = f.post("/v1/recommend", huge);
  CHECK(code_of(r) == "unknown_model");
  CHECK(r.body.size() < 200);  // the name is not echoed back in full

  // A contract model name that this artifact does not hold.
  recs::service::BundleHolder pop_only;
  pop_only.store(recs::testing::fixture_bundle({"pop", "pop_count"}));
  recs::service::Service svc(pop_only, f.metrics, [](std::string_view) {});
  json knn = rec(json::array());
  knn["model"] = "knn";
  CHECK(code_of(svc.handle({"POST", "/v1/recommend", {}, "", knn.dump()})) == "unknown_model");
  CHECK(code_of(svc.handle({"GET", "/v1/similar/260", {{"model", "knn"}}, "", ""})) == "unknown_model");
}

TEST_CASE("compare: models list rules", "[model-service][validation]") {
  Fixture f;
  const auto cmp = [&](json models) {
    return f.post("/v1/compare", json{{"models", std::move(models)}, {"ratings", json::array()}, {"k", 5}});
  };
  CHECK(cmp({"pop", "knn", "content", "mf", "als"}).status == 200);
  CHECK(code_of(cmp(json::array())) == "bad_request");
  CHECK(code_of(cmp({"pop", "knn", "content", "mf", "als", "pop_count"})) == "bad_request");
  CHECK(code_of(cmp({"pop", "pop"})) == "bad_request");
  CHECK(code_of(cmp({"pop", 3})) == "bad_request");
  CHECK(code_of(cmp("pop")) == "bad_request");
  CHECK(code_of(cmp({"pop", "svd"})) == "unknown_model");
  const json ok = json::parse(cmp({"als", "pop"}).body);
  CHECK(ok["results"][0]["model"] == "als");
  CHECK(ok["results"][1]["model"] == "pop");
}

TEST_CASE("similar: path and query validation", "[model-service][validation]") {
  Fixture f;
  CHECK(f.get("/v1/similar/260").status == 200);
  CHECK(json::parse(f.get("/v1/similar/260").body)["items"].size() == 16);  // default k = 20, 17 movies
  CHECK(json::parse(f.get("/v1/similar/260", {{"model", "content"}, {"k", "100"}}).body)["model"] == "content");
  for (const char* id : {"0", "-1", "abc", "2147483648", "99999999999999999999", "26a", "", "+5", " 5", "1.0"}) {
    INFO(id);
    const auto r = f.get(std::string("/v1/similar/") + id);
    CHECK(r.status == 400);
    CHECK(code_of(r) == "bad_request");
  }
  CHECK(f.get("/v1/similar/2147483647").status == 404);
  for (const char* k : {"0", "101", "x", "", "-3", "3.5", "007x"}) {
    INFO(k);
    CHECK(code_of(f.get("/v1/similar/260", {{"k", k}})) == "bad_request");
  }
  CHECK(f.get("/v1/similar/260", {{"k", "007"}}).status == 200);
  CHECK(code_of(f.get("/v1/similar/260", {{"model", "pop"}})) == "unknown_model");
  CHECK(code_of(f.get("/v1/similar/260", {{"k", "3"}, {"k", "4"}})) == "bad_request");
  CHECK(f.get("/v1/similar/260/extra").status == 404);
}

TEST_CASE("search: q and limit validation", "[model-service][validation]") {
  Fixture f;
  CHECK(code_of(f.get("/v1/movies/search")) == "bad_request");
  CHECK(code_of(f.get("/v1/movies/search", {{"q", ""}})) == "bad_request");
  CHECK(f.get("/v1/movies/search", {{"q", std::string(100, 'a')}}).status == 200);
  CHECK(code_of(f.get("/v1/movies/search", {{"q", std::string(101, 'a')}})) == "bad_request");
  std::string accented;
  for (int i = 0; i < 100; ++i) accented += "\xc3\xa9";  // 100 x 'é' = 200 bytes, 100 characters
  CHECK(f.get("/v1/movies/search", {{"q", accented}}).status == 200);
  CHECK(code_of(f.get("/v1/movies/search", {{"q", "star\xff"}})) == "bad_request");
  CHECK(code_of(f.get("/v1/movies/search", {{"q", "\xc3"}})) == "bad_request");
  CHECK(f.get("/v1/movies/search", {{"q", std::string("a\0b", 3)}}).status == 200);
  CHECK(json::parse(f.get("/v1/movies/search", {{"q", "!!!"}}).body)["movie_ids"].empty());
  for (const char* l : {"0", "51", "x", "-1", ""}) {
    INFO(l);
    CHECK(code_of(f.get("/v1/movies/search", {{"q", "star"}, {"limit", l}})) == "bad_request");
  }
  CHECK(f.get("/v1/movies/search", {{"q", "a"}, {"limit", "50"}}).status == 200);
}

TEST_CASE("movies/batch and onboarding limits", "[model-service][validation]") {
  Fixture f;
  json ids = json::array();
  for (int i = 1; i <= 200; ++i) ids.push_back(i);
  CHECK(f.post("/v1/movies/batch", json{{"ids", ids}}).status == 200);
  ids.push_back(201);
  CHECK(code_of(f.post("/v1/movies/batch", json{{"ids", ids}})) == "bad_request");
  CHECK(code_of(f.post("/v1/movies/batch", json{{"ids", {0}}})) == "bad_request");
  CHECK(code_of(f.post("/v1/movies/batch", json{{"ids", {260}}, {"x", 1}})) == "bad_request");
  CHECK(code_of(f.post("/v1/movies/batch", json::object())) == "bad_request");

  CHECK(f.post("/v1/onboarding", json{{"n", 100}}).status == 200);
  CHECK(code_of(f.post("/v1/onboarding", json{{"n", 101}})) == "bad_request");
  CHECK(code_of(f.post("/v1/onboarding", json{{"n", "4"}})) == "bad_request");
  CHECK(code_of(f.post("/v1/onboarding", json{{"n", 4}, {"exclude", {-4}}})) == "bad_request");
}

TEST_CASE("hostile bodies are rejected cleanly", "[model-service][validation]") {
  Fixture f;
  // Deep nesting would overflow the stack of a recursive parser.
  const std::string deep = std::string(500000, '[') + std::string(500000, ']');
  CHECK(code_of(f.post("/v1/recommend", deep)) == "invalid_json");
  const std::string deep_obj = "{\"model\":" + std::string(100000, '[');
  CHECK(code_of(f.post("/v1/compare", deep_obj)) == "invalid_json");
  // Nesting inside a string does not count.
  json s = rec(json::array());
  s["model"] = std::string(1000, '[');
  CHECK(code_of(f.post("/v1/recommend", s)) == "unknown_model");
  // Over 1 MiB.
  const std::string big = "{\"ids\":[" + std::string((1u << 20), ' ') + "1]}";
  const auto r = f.post("/v1/movies/batch", big);
  CHECK(r.status == 400);
  CHECK(code_of(r) == "body_too_large");
  // Huge numbers, NaN-like tokens, nulls.
  CHECK(code_of(f.post("/v1/recommend", std::string(R"({"model":"pop","ratings":[],"k":1e999})"))) != "internal");
  CHECK(code_of(f.post("/v1/recommend", std::string(R"({"model":"pop","ratings":[],"k":NaN})"))) == "invalid_json");
  CHECK(code_of(f.post("/v1/recommend", std::string(R"({"model":"pop","ratings":null,"k":1})"))) == "bad_request");
}

TEST_CASE("routing: unknown paths and methods", "[model-service][validation]") {
  Fixture f;
  auto r = f.get("/v2/recommend");
  CHECK(r.status == 404);
  CHECK(code_of(r) == "not_found");
  CHECK(r.model_version == recs::testing::kFixtureVersion);
  auto m = f.svc.handle({"DELETE", "/v1/recommend", {}, "", ""});
  CHECK(m.status == 405);
  CHECK(m.allow == "POST");
  CHECK(code_of(m) == "bad_request");
  CHECK(f.svc.handle({"POST", "/v1/version", {}, "", ""}).status == 405);
  CHECK(f.svc.handle({"GET", "/v1/recommend", {}, "", ""}).allow == "POST");
}
