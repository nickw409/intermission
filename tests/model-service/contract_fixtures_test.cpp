// Gate: recs-model produces every response in contracts/fixtures/model/.
//
// Each fixture case is replayed against Service::handle(): with the fixture artifact loaded, or
// with nothing loaded for the 503 cases. Checked for every case: the status code, X-Model-Version,
// the error code and message, and that the body validates against the operation's schema in
// contracts/model.yaml. Where the value is determined by the catalog alone (movies/batch, search,
// version, models, healthz) the body must equal the fixture exactly; for model output (recommend,
// compare, similar, onboarding) the numbers depend on the trained model, so the test checks shape
// and invariants (k, exclusions, `ignored`, model names) instead (contracts/README.md).
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "fixtures.hpp"
#include "schema.hpp"
#include "service.hpp"
#include "test_bundle.hpp"

namespace {

using nlohmann::json;
using recs::service::Request;
using recs::service::Response;

json read_json(const std::filesystem::path& p) {
  std::ifstream in(p);
  REQUIRE(in);
  return json::parse(in);
}

std::string read_text(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// What may follow `prop: { $ref: '<ref>'` on a model.yaml request-property line: either just the
// closing ` }`, or `, description: <text> }` where <text> is one YAML scalar and nothing else.
// A plain (unquoted) scalar in a flow mapping ends at ", ", so a plain description without ", " cannot
// hide another keyword; a single-quoted one must close right before " }" with only doubled quotes
// ('') inside.
bool ref_line_tail_ok(const std::string& tail) {
  if (tail == " }") return true;
  const std::string lead = ", description: ";
  const std::string close = " }";
  if (tail.size() < lead.size() + close.size() + 1 || tail.compare(0, lead.size(), lead) != 0 ||
      tail.compare(tail.size() - close.size(), close.size(), close) != 0) {
    return false;
  }
  const std::string text = tail.substr(lead.size(), tail.size() - lead.size() - close.size());
  if (text.front() != '\'') return text.find(", ") == std::string::npos && text.find('{') == std::string::npos;
  if (text.size() < 2 || text.back() != '\'') return false;
  const std::string inner = text.substr(1, text.size() - 2);
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] != '\'') continue;
    if (i + 1 >= inner.size() || inner[i + 1] != '\'') return false;  // a lone quote ends the scalar
    ++i;
  }
  return true;
}

Request to_request(const json& r) {
  Request req;
  req.method = r.at("method");
  req.path = r.at("path");
  const json query = r.value("query", json::object());
  for (const auto& [k, v] : query.items()) req.query.emplace_back(k, v.get<std::string>());
  if (r.contains("rawBody")) {
    req.body = r["rawBody"].get<std::string>();
  } else if (r.contains("body") && !r["body"].is_null()) {
    req.body = r["body"].dump();
  }
  return req;
}

// The success schema of each operation.
const std::map<std::string, std::string>& ok_schema() {
  static const std::map<std::string, std::string> m = {
      {"recommend", "RecommendResponse"}, {"compare", "CompareResponse"}, {"similar", "SimilarResponse"},
      {"onboarding", "IdList"},          {"search", "IdList"},           {"moviesBatch", "BatchResponse"},
      {"version", "Version"},            {"models", "ModelsResponse"},   {"healthz", "Health"},
  };
  return m;
}

struct Harness {
  recs::service::BundleHolder loaded;
  recs::service::BundleHolder empty;
  recs::service::Metrics metrics;
  recs::service::Service with_artifact{loaded, metrics, [](std::string_view) {}};
  recs::service::Service without_artifact{empty, metrics, [](std::string_view) {}};
  Harness() { loaded.store(recs::testing::fixture_bundle()); }
};

std::set<std::int64_t> ids_of(const json& arr) {
  std::set<std::int64_t> s;
  for (const auto& v : arr) s.insert(v.get<std::int64_t>());
  return s;
}

// The fixture catalog's row for `id`.
const recs::testing::CatalogRow& catalog_row(std::int64_t id) {
  static const auto rows = recs::testing::fixture_catalog_rows();
  const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.id == id; });
  REQUIRE(it != rows.end());
  return *it;
}

// Invariants of an item list produced for request body `body`, including its genre and year filters.
void check_items(const json& items, const json& body) {
  CHECK(items.size() <= body.at("k").get<std::size_t>());
  std::set<std::int64_t> rated;
  for (const auto& r : body.at("ratings")) rated.insert(r.at("movie_id").get<std::int64_t>());
  const auto excluded = ids_of(body.value("exclude", json::array()));
  std::set<std::int64_t> seen;
  for (const auto& it : items) {
    const auto id = it.at("movie_id").get<std::int64_t>();
    CHECK_FALSE(rated.contains(id));
    CHECK_FALSE(excluded.contains(id));
    CHECK(seen.insert(id).second);
    for (const auto& b : it.at("because")) CHECK(rated.contains(b.at("movie_id").get<std::int64_t>()));
    const auto& row = catalog_row(id);
    if (body.contains("genres") && !body["genres"].empty()) {
      CHECK(std::any_of(body["genres"].begin(), body["genres"].end(), [&](const json& g) {
        return std::find(row.genres.begin(), row.genres.end(), g.get<std::string>()) != row.genres.end();
      }));
    }
    if (body.contains("year_from") || body.contains("year_to")) {
      REQUIRE(row.year.has_value());
      if (body.contains("year_from")) CHECK(*row.year >= body["year_from"].get<int>());
      if (body.contains("year_to")) CHECK(*row.year <= body["year_to"].get<int>());
    }
  }
}

void check_case(Harness& h, const std::string& op, const json& c) {
  const json& expected = c.at("response");
  const int status = expected.at("status");
  const Request req = to_request(c.at("request"));
  const Response res = (status == 503 ? h.without_artifact : h.with_artifact).handle(req);

  REQUIRE(res.status == status);
  CHECK(res.content_type == "application/json");
  CHECK(res.model_version == expected.at("headers").at("X-Model-Version").get<std::string>());
  CHECK_FALSE(res.request_id.empty());

  const json body = json::parse(res.body);
  const json& want = expected.at("body");
  if (want.contains("error")) {
    CHECK(recs::testing::validate_against(body, "Error").empty());
    CHECK(body == want);
    return;
  }
  const auto errors = recs::testing::validate_against(body, ok_schema().at(op));
  INFO(json(errors).dump());
  CHECK(errors.empty());

  if (op == "moviesBatch" || op == "search" || op == "version" || op == "models" || op == "healthz") {
    CHECK(body == want);
  } else if (op == "recommend") {
    const json rq = json::parse(req.body);
    CHECK(body.at("model") == want.at("model"));
    CHECK(body.at("ignored") == want.at("ignored"));
    CHECK(body.at("items").size() == want.at("items").size());
    check_items(body.at("items"), rq);
  } else if (op == "compare") {
    const json rq = json::parse(req.body);
    CHECK(body.at("ignored") == want.at("ignored"));
    REQUIRE(body.at("results").size() == want.at("results").size());
    for (std::size_t i = 0; i < body["results"].size(); ++i) {
      CHECK(body["results"][i].at("model") == want["results"][i].at("model"));
      CHECK(body["results"][i].at("items").size() == want["results"][i].at("items").size());
      check_items(body["results"][i].at("items"), rq);
    }
  } else if (op == "similar") {
    CHECK(body.at("model") == want.at("model"));
    CHECK(body.at("movie_id") == want.at("movie_id"));
    CHECK(body.at("items").size() == want.at("items").size());
    for (const auto& it : body.at("items")) CHECK(it.at("movie_id") != body.at("movie_id"));
  } else if (op == "onboarding") {
    const json rq = json::parse(req.body);
    CHECK(body.at("movie_ids").size() == want.at("movie_ids").size());
    const auto excluded = ids_of(rq.value("exclude", json::array()));
    const auto got = ids_of(body.at("movie_ids"));
    CHECK(got.size() == body.at("movie_ids").size());
    for (auto id : got) CHECK_FALSE(excluded.contains(id));
  } else {
    FAIL("unhandled operation " << op);
  }
}

}  // namespace

TEST_CASE("recs-model: produces every contracts/fixtures/model response", "[model-service][contract]") {
  Harness h;
  const auto dir = recs::testing::contracts_dir() / "fixtures" / "model";
  std::size_t files = 0;
  std::size_t cases = 0;
  std::set<std::string> ops;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() != ".json") continue;
    const json f = read_json(e.path());
    const std::string op = f.at("operationId");
    ops.insert(op);
    ++files;
    for (const auto& c : f.at("cases")) {
      DYNAMIC_SECTION(op << "/" << c.at("name").get<std::string>()) { check_case(h, op, c); }
      ++cases;
    }
  }
  // Every operation in model.yaml except /metrics (text, no fixture) has a fixture file.
  CHECK(ops == std::set<std::string>{"compare", "healthz", "models", "moviesBatch", "onboarding", "recommend",
                                     "search", "similar", "version"});
  CHECK(files == 9);
  CHECK(cases >= 40);
}

TEST_CASE("recs-model: fixture responses validate against the transcribed schemas", "[model-service][contract]") {
  // Guards the schema transcription in schema.hpp: the fixtures are validated against model.yaml
  // in CI (tools/contracts), so they must also pass here.
  const auto dir = recs::testing::contracts_dir() / "fixtures" / "model";
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const json f = read_json(e.path());
    const std::string op = f.at("operationId");
    for (const auto& c : f.at("cases")) {
      const json& body = c.at("response").at("body");
      const std::string schema = body.contains("error") ? "Error" : ok_schema().at(op);
      const auto errors = recs::testing::validate_against(body, schema);
      INFO(op << "/" << c.at("name").get<std::string>() << ": " << json(errors).dump());
      CHECK(errors.empty());
    }
  }
}

TEST_CASE("recs-model: transcribed schemas match model.yaml", "[model-service][contract]") {
  const std::string yaml = read_text(recs::testing::contracts_dir() / "model.yaml");
  const auto& components = recs::testing::model_components();
  const auto list_line = [](std::string_view key, const json& values) {
    std::string line = std::string(key) + ": [";
    for (std::size_t i = 0; i < values.size(); ++i) line += (i ? ", " : "") + values[i].get<std::string>();
    return line + "]";
  };

  // Every transcribed `required` and `enum` list appears verbatim in model.yaml.
  std::size_t required_checked = 0;
  std::size_t enums_checked = 0;
  const auto check_lists = [&](const json& schema, const auto& self) -> void {
    if (!schema.is_object()) return;
    for (const std::string_view key : {"required", "enum"}) {
      if (!schema.contains(key)) continue;
      const std::string line = list_line(key, schema[std::string(key)]);
      INFO(line);
      CHECK(yaml.find(line) != std::string::npos);
      ++(key == "required" ? required_checked : enums_checked);
    }
    for (const auto& [k, v] : schema.items()) self(v, self);
  };
  for (const auto& [name, schema] : components.items()) check_lists(schema, check_lists);
  CHECK(required_checked >= 17);
  CHECK(enums_checked >= 5);

  // The request schemas: every property that is a $ref appears as that line in its own schema's
  // block of model.yaml. After the $ref the line may carry a description and nothing else, so an
  // added keyword (a tighter maximum, say), which applies next to a $ref in OpenAPI 3.1, is caught.
  // The genre list's and release year's bounds are checked below.
  for (const std::string req : {"RecommendRequest", "CompareRequest"}) {
    const std::size_t start = yaml.find("\n    " + req + ":\n");
    REQUIRE(start != std::string::npos);
    std::size_t end = start + 1;  // the next schema: a line indented exactly four spaces
    do {
      end = yaml.find("\n    ", end + 1);
    } while (end != std::string::npos && end + 5 < yaml.size() && yaml[end + 5] == ' ');
    const std::string block = yaml.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const json& props = components.at(req).at("properties");
    CHECK(props.contains("genres"));
    CHECK(props.contains("year_from"));
    CHECK(props.contains("year_to"));
    for (const auto& [prop, sub] : props.items()) {
      if (!sub.contains("$ref")) continue;
      const std::string head = "\n        " + prop + ": { $ref: '" + sub["$ref"].get<std::string>() + "'";
      INFO(req << ": " << head.substr(1));
      const std::size_t at = block.find(head);
      REQUIRE(at != std::string::npos);
      const std::size_t line_end = block.find('\n', at + head.size());
      CHECK(ref_line_tail_ok(block.substr(at + head.size(), line_end - (at + head.size()))));
    }
  }
  const json& genres = components.at("Genres");
  CHECK(genres.at("maxItems") == 19);
  CHECK(genres.at("uniqueItems") == true);
  CHECK(components.at("Genre").at("enum").size() == 19);
  CHECK(yaml.find("    Genres:\n      type: array\n      maxItems: 19\n      uniqueItems: true\n") != std::string::npos);
  CHECK(yaml.find("items: { $ref: '#/components/schemas/Genre' }") != std::string::npos);
  const json& year = components.at("ReleaseYear");
  CHECK(year.at("minimum") == 1800);
  CHECK(year.at("maximum") == 2100);
  CHECK(yaml.find("    ReleaseYear:\n      type: integer\n      minimum: 1800\n      maximum: 2100\n") != std::string::npos);
}
