// The optional release-year range of /v1/recommend and /v1/compare (contracts/model.yaml:
// ReleaseYear, year_from, year_to).
//
// Validation: each bound, when present, is a JSON Schema integer in [1800, 2100] (so 1990.0 counts,
// 1990.5, "1990", true and null do not); anything else is a 400 bad_request. year_from > year_to is
// a 400 bad_request with the exact message "year_from must not exceed year_to".
//
// Behaviour: both bounds are inclusive and either may be given alone. When at least one bound is
// present, movies without a parsed year are not returned; with no bound they are ordinary
// candidates. Like the genre filter, the range is applied BEFORE the top-k cut, for every model:
// the result equals the unfiltered full ranking with out-of-range movies removed, then cut to k.
// Genres and years combine as AND.
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "schema.hpp"
#include "service.hpp"
#include "test_bundle.hpp"

namespace {

using nlohmann::json;
using recs::service::Response;

const std::vector<std::string> kModels = {"pop", "pop_count", "knn", "content", "mf", "als"};

struct Fixture {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc{holder, metrics, [](std::string_view) {}};
  Fixture() { holder.store(recs::testing::fixture_bundle()); }

  Response post(const std::string& path, const json& body) { return svc.handle({"POST", path, {}, "", body.dump()}); }
};

json error_of(const Response& r) {
  const json b = json::parse(r.body);
  INFO(r.body);
  REQUIRE(recs::testing::validate_against(b, "Error").empty());
  return b["error"];
}

struct Movie {
  std::optional<int> year;
  std::vector<std::string> genres;
};

// movie id -> year and genres, from the fixture catalog.
const std::map<std::int64_t, Movie>& catalog() {
  static const auto m = [] {
    std::map<std::int64_t, Movie> out;
    for (const auto& r : recs::testing::fixture_catalog_rows()) out[r.id] = {r.year, r.genres};
    return out;
  }();
  return m;
}

struct Range {
  std::optional<int> from;
  std::optional<int> to;
};

bool in_range(std::int64_t id, const Range& r) {
  const auto& y = catalog().at(id).year;
  if (!r.from && !r.to) return true;
  if (!y) return false;
  return (!r.from || *y >= *r.from) && (!r.to || *y <= *r.to);
}

bool has_genre(std::int64_t id, const std::vector<std::string>& wanted) {
  if (wanted.empty()) return true;
  const auto& g = catalog().at(id).genres;
  return std::any_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return std::find(g.begin(), g.end(), w) != g.end(); });
}

json some_ratings() { return json::array({{{"movie_id", 260}, {"rating", 5}}, {{"movie_id", 1}, {"rating", 4}}}); }

json with_range(json b, const Range& r) {
  if (r.from) b["year_from"] = *r.from;
  if (r.to) b["year_to"] = *r.to;
  return b;
}

json rec_body(const std::string& model, std::size_t k, const Range& r = {}) {
  return with_range({{"model", model}, {"ratings", some_ratings()}, {"k", k}}, r);
}

json items_of(Fixture& f, const json& body) {
  const auto r = f.post("/v1/recommend", body);
  INFO(body.dump() << " -> " << r.body);
  REQUIRE(r.status == 200);
  const json b = json::parse(r.body);
  REQUIRE(recs::testing::validate_against(b, "RecommendResponse").empty());
  return b["items"];
}

// The unfiltered full ranking with movies outside the range (and, if given, the genres) dropped, cut to k.
json expected_filtered(const json& full, const Range& r, std::size_t k, const std::vector<std::string>& genres = {}) {
  json out = json::array();
  for (const auto& it : full) {
    if (out.size() == k) break;
    const auto id = it["movie_id"].get<std::int64_t>();
    if (in_range(id, r) && has_genre(id, genres)) out.push_back(it);
  }
  return out;
}

std::vector<Range> ranges() {
  return {{1990, 1999}, {1994, 1994}, {1999, std::nullopt}, {std::nullopt, 1980}, {1800, 2100},
          {2002, 2019}, {1958, 1958}, {2001, 2001}, {1977, 1983}, {std::nullopt, 2100}, {1800, std::nullopt}};
}

std::string describe(const Range& r) {
  return "[" + (r.from ? std::to_string(*r.from) : "-") + ", " + (r.to ? std::to_string(*r.to) : "-") + "]";
}

}  // namespace

// ---- validation -----------------------------------------------------------------------------------

TEST_CASE("recs-model: years: invalid bounds are 400 bad_request", "[model-service][validation][years]") {
  Fixture f;
  const std::vector<json> bad = {
      json(1799), json(2101), json(0), json(-1990), json(10000), json(1990.5), json(1799.0), json(2100.5),
      json(1e300), json(18446744073709551615ULL), json(-9223372036854775807LL - 1),
      json("1990"), json(true), json(false), json(nullptr), json::array({1990}), json::object(),
  };
  for (const char* field : {"year_from", "year_to"}) {
    const std::string expected = std::string(field) + " must be an integer between 1800 and 2100";
    for (const json& v : bad) {
      INFO(field << " = " << v.dump());
      json body = rec_body("pop", 5);
      body[field] = v;
      const auto r = f.post("/v1/recommend", body);
      CHECK(r.status == 400);
      const json e = error_of(r);
      CHECK(e["code"] == "bad_request");
      CHECK(e["message"] == expected);
      json cmp = {{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 5}, {field, v}};
      const auto c = f.post("/v1/compare", cmp);
      CHECK(c.status == 400);
      const json ce = error_of(c);
      CHECK(ce["code"] == "bad_request");
      CHECK(ce["message"] == expected);
    }
  }
}

TEST_CASE("recs-model: years: year_from > year_to is 400 with the contract message", "[model-service][validation][years]") {
  Fixture f;
  for (const Range& r : std::vector<Range>{{2000, 1990}, {1801, 1800}, {2100, 1800}}) {
    INFO(describe(r));
    const auto rec = f.post("/v1/recommend", rec_body("pop", 5, r));
    CHECK(rec.status == 400);
    const json e = error_of(rec);
    CHECK(e["code"] == "bad_request");
    CHECK(e["message"] == "year_from must not exceed year_to");
    json cmp = with_range({{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 5}}, r);
    const auto c = f.post("/v1/compare", cmp);
    CHECK(c.status == 400);
    CHECK(error_of(c)["message"] == "year_from must not exceed year_to");
  }
}

TEST_CASE("recs-model: years: the extreme and integral-float bounds are accepted", "[model-service][validation][years]") {
  Fixture f;
  for (const json& body : {rec_body("pop", 5, {1800, 2100}), rec_body("pop", 5, {1800, 1800}), rec_body("pop", 5, {2100, 2100}),
                           json{{"model", "pop"}, {"ratings", some_ratings()}, {"k", 5}, {"year_from", 1990.0}, {"year_to", 1999.0}}}) {
    INFO(body.dump());
    CHECK(f.post("/v1/recommend", body).status == 200);
  }
  // 1990.0 is the integer 1990 (JSON Schema "integer"): same result as 1990.
  const json a = items_of(f, json{{"model", "pop"}, {"ratings", some_ratings()}, {"k", 10}, {"year_from", 1990.0}, {"year_to", 1999.0}});
  CHECK(a == items_of(f, rec_body("pop", 10, {1990, 1999})));
}

// ---- behaviour ------------------------------------------------------------------------------------

TEST_CASE("recs-model: years: result is the filtered full ranking, for every model", "[model-service][years]") {
  Fixture f;
  for (const auto& m : kModels) {
    const json full = items_of(f, rec_body(m, 100));
    REQUIRE(full.size() >= 10);
    for (const Range& r : ranges()) {
      for (std::size_t k : {1u, 2u, 3u, 5u, 100u}) {
        INFO(m << " " << describe(r) << " k=" << k);
        const json got = items_of(f, rec_body(m, k, r));
        CHECK(got == expected_filtered(full, r, k));
        for (const auto& it : got) CHECK(in_range(it["movie_id"].get<std::int64_t>(), r));
      }
    }
  }
}

TEST_CASE("recs-model: years: bounds are inclusive at both ends", "[model-service][years]") {
  Fixture f;
  // Pulp Fiction, Forrest Gump and Shawshank are 1994; Lone Star (1081) is 1996; Matrix and Fight Club
  // are 1999; Jurassic Park (480) is 1993, just outside.
  const json got = items_of(f, rec_body("pop", 100, {1994, 1999}));
  std::vector<std::int64_t> ids;
  for (const auto& it : got) ids.push_back(it["movie_id"].get<std::int64_t>());
  for (std::int64_t id : {296, 356, 318, 2571, 2959, 1081, 480 /*1993: out*/}) {
    INFO(id);
    const bool present = std::find(ids.begin(), ids.end(), id) != ids.end();
    CHECK(present == (id != 480));
  }
  // A one-year range returns exactly that year's movies.
  const json y99 = items_of(f, rec_body("pop", 100, {1999, 1999}));
  std::vector<std::int64_t> got99;
  for (const auto& it : y99) got99.push_back(it["movie_id"].get<std::int64_t>());
  std::sort(got99.begin(), got99.end());
  CHECK(got99 == std::vector<std::int64_t>{2571, 2959});
}

TEST_CASE("recs-model: years: one-sided bounds", "[model-service][years]") {
  Fixture f;
  // Only year_to: 260 (1977, but rated), 1196 (1980), 7003 (1958).
  std::vector<std::int64_t> early;
  for (const auto& it : items_of(f, rec_body("pop", 100, {std::nullopt, 1980}))) early.push_back(it["movie_id"]);
  std::sort(early.begin(), early.end());
  CHECK(early == std::vector<std::int64_t>{1196, 7003});
  // Only year_from: 4993 and 4973 (2001), 7002 (2020).
  std::vector<std::int64_t> late;
  for (const auto& it : items_of(f, rec_body("pop", 100, {2001, std::nullopt}))) late.push_back(it["movie_id"]);
  std::sort(late.begin(), late.end());
  CHECK(late == std::vector<std::int64_t>{4973, 4993, 7002});
}

TEST_CASE("recs-model: years: movies without a year are excluded only under a bound", "[model-service][years]") {
  Fixture f;
  // 7001 (Hamlet) has no parsed year.
  const auto contains_7001 = [](const json& items) {
    return std::any_of(items.begin(), items.end(), [](const json& it) { return it["movie_id"] == 7001; });
  };
  for (const auto& m : kModels) {
    INFO(m);
    CHECK(contains_7001(items_of(f, rec_body(m, 100))));
    CHECK_FALSE(contains_7001(items_of(f, rec_body(m, 100, {1800, 2100}))));
    CHECK_FALSE(contains_7001(items_of(f, rec_body(m, 100, {1800, std::nullopt}))));
    CHECK_FALSE(contains_7001(items_of(f, rec_body(m, 100, {std::nullopt, 2100}))));
    // The widest range removes exactly the yearless movie.
    json full = items_of(f, rec_body(m, 100));
    json widest = items_of(f, rec_body(m, 100, {1800, 2100}));
    CHECK(widest.size() + 1 == full.size());
  }
}

TEST_CASE("recs-model: years: an empty range returns no items", "[model-service][years]") {
  Fixture f;
  for (const auto& m : kModels) {
    INFO(m);
    CHECK(items_of(f, rec_body(m, 10, {2002, 2019})) == json::array());
    CHECK(items_of(f, rec_body(m, 10, {1800, 1900})) == json::array());
  }
}

TEST_CASE("recs-model: years: combine with genres as AND", "[model-service][years][genres]") {
  Fixture f;
  const std::vector<std::pair<std::vector<std::string>, Range>> cases = {
      {{"Drama"}, {1990, 1999}},  {{"Sci-Fi"}, {1980, std::nullopt}}, {{"Comedy", "Romance"}, {2000, 2100}},
      {{"Thriller"}, {1994, 1994}}, {{"Documentary"}, {1990, 2000}}, {{"Drama"}, {std::nullopt, 1960}},
  };
  for (const auto& m : kModels) {
    const json full = items_of(f, rec_body(m, 100));
    for (const auto& [genres, r] : cases) {
      for (std::size_t k : {1u, 3u, 100u}) {
        INFO(m << " " << json(genres).dump() << " " << describe(r) << " k=" << k);
        json body = rec_body(m, k, r);
        body["genres"] = genres;
        const json got = items_of(f, body);
        CHECK(got == expected_filtered(full, r, k, genres));
      }
    }
  }
  // Explicit: Drama in the 1990s is 2959, 296, 356, 318, 1081 minus nothing rated.
  json body = rec_body("pop", 100, {1990, 1999});
  body["genres"] = {"Drama"};
  std::vector<std::int64_t> ids;
  for (const auto& it : items_of(f, body)) ids.push_back(it["movie_id"]);
  std::sort(ids.begin(), ids.end());
  CHECK(ids == std::vector<std::int64_t>{296, 318, 356, 1081, 2959});
}

TEST_CASE("recs-model: years: exclude still applies inside the range", "[model-service][years]") {
  Fixture f;
  for (const auto& m : kModels) {
    INFO(m);
    const json full = items_of(f, rec_body(m, 100));
    json body = rec_body(m, 3, {1990, 1999});
    body["exclude"] = {2959, 296};
    const json got = items_of(f, body);
    json expected = json::array();
    for (const auto& it : full) {
      if (expected.size() == 3) break;
      const auto id = it["movie_id"].get<std::int64_t>();
      if (id != 2959 && id != 296 && in_range(id, {1990, 1999})) expected.push_back(it);
    }
    CHECK(got == expected);
  }
}

TEST_CASE("recs-model: years: compare filters every model like recommend", "[model-service][years]") {
  Fixture f;
  // compare takes at most 5 models, so the six are sent in two groups.
  const std::vector<std::vector<std::string>> groups = {{"pop", "pop_count", "knn"}, {"content", "mf", "als"}};
  for (const Range& r : ranges()) {
    for (const auto& models : groups) {
      INFO(describe(r) << " " << json(models).dump());
      json cmp = with_range({{"models", models}, {"ratings", some_ratings()}, {"k", 4}, {"genres", {"Drama", "Sci-Fi"}}}, r);
      const auto res = f.post("/v1/compare", cmp);
      REQUIRE(res.status == 200);
      const json b = json::parse(res.body);
      REQUIRE(recs::testing::validate_against(b, "CompareResponse").empty());
      REQUIRE(b["results"].size() == models.size());
      for (std::size_t i = 0; i < models.size(); ++i) {
        json rec = rec_body(models[i], 4, r);
        rec["genres"] = {"Drama", "Sci-Fi"};
        CHECK(b["results"][i]["items"] == items_of(f, rec));
      }
    }
  }
}

TEST_CASE("recs-model: years: request schemas accept and reject like the service", "[model-service][contract][years]") {
  using recs::testing::validate_against;
  CHECK(validate_against(rec_body("pop", 5, {1990, 1999}), "RecommendRequest").empty());
  CHECK(validate_against(rec_body("pop", 5, {1800, std::nullopt}), "RecommendRequest").empty());
  CHECK(validate_against(rec_body("pop", 5, {std::nullopt, 2100}), "RecommendRequest").empty());
  CHECK_FALSE(validate_against(rec_body("pop", 5, {1799, std::nullopt}), "RecommendRequest").empty());
  CHECK_FALSE(validate_against(rec_body("pop", 5, {std::nullopt, 2101}), "RecommendRequest").empty());
  json s = rec_body("pop", 5);
  s["year_from"] = "1990";
  CHECK_FALSE(validate_against(s, "RecommendRequest").empty());
  json cmp = with_range({{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 5}}, {1990, 1999});
  CHECK(validate_against(cmp, "CompareRequest").empty());
  cmp["year_to"] = 1990.5;
  CHECK_FALSE(validate_against(cmp, "CompareRequest").empty());
}
