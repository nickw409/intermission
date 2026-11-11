// The optional `genres` filter of /v1/recommend and /v1/compare (contracts/model.yaml: Genres).
//
// Validation: an array of distinct MovieLens genre names, at most 19; anything else is a 400
// bad_request. Absent and empty both mean "no filter".
//
// Behaviour: only movies with at least one listed genre are returned, for every loaded model, and
// the filter is applied BEFORE the top-k cut: the result equals the unfiltered full ranking with
// non-matching movies removed, then cut to k. So a genre whose movies all rank below k unfiltered
// still returns them, and fewer than k items come back only when fewer than k movies match.
#include <algorithm>
#include <map>
#include <set>
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

const std::vector<std::string> kAllGenres = {"Action",  "Adventure", "Animation", "Children", "Comedy",
                                             "Crime",   "Documentary", "Drama",   "Fantasy",  "Film-Noir",
                                             "Horror",  "IMAX",      "Musical",   "Mystery",  "Romance",
                                             "Sci-Fi",  "Thriller",  "War",       "Western"};

const std::vector<std::string> kModels = {"pop", "pop_count", "knn", "content", "mf", "als"};

struct Fixture {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc{holder, metrics, [](std::string_view) {}};
  Fixture() { holder.store(recs::testing::fixture_bundle()); }

  Response post(const std::string& path, const json& body) { return svc.handle({"POST", path, {}, "", body.dump()}); }
};

std::string code_of(const Response& r) {
  const json b = json::parse(r.body);
  INFO(r.body);
  REQUIRE(recs::testing::validate_against(b, "Error").empty());
  return b["error"]["code"];
}

// movie id -> genres, from the fixture catalog.
const std::map<std::int64_t, std::vector<std::string>>& catalog_genres() {
  static const auto m = [] {
    std::map<std::int64_t, std::vector<std::string>> out;
    for (const auto& r : recs::testing::fixture_catalog_rows()) out[r.id] = r.genres;
    return out;
  }();
  return m;
}

bool matches(std::int64_t id, const std::vector<std::string>& wanted) {
  const auto& g = catalog_genres().at(id);
  return std::any_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return std::find(g.begin(), g.end(), w) != g.end(); });
}

// Some ratings so that the personalised stand-ins produce a non-trivial ranking.
json some_ratings() { return json::array({{{"movie_id", 260}, {"rating", 5}}, {{"movie_id", 1}, {"rating", 4}}}); }

json rec_body(const std::string& model, std::size_t k, const json& genres, const json& exclude = nullptr) {
  json b = {{"model", model}, {"ratings", some_ratings()}, {"k", k}};
  if (!genres.is_null()) b["genres"] = genres;
  if (!exclude.is_null()) b["exclude"] = exclude;
  return b;
}

json items_of(Fixture& f, const json& body) {
  const auto r = f.post("/v1/recommend", body);
  INFO(body.dump() << " -> " << r.body);
  REQUIRE(r.status == 200);
  const json b = json::parse(r.body);
  REQUIRE(recs::testing::validate_against(b, "RecommendResponse").empty());
  return b["items"];
}

// The unfiltered full ranking, with non-matching movies dropped, cut to k.
json expected_filtered(const json& full, const std::vector<std::string>& genres, std::size_t k) {
  json out = json::array();
  for (const auto& it : full) {
    if (out.size() == k) break;
    if (matches(it["movie_id"].get<std::int64_t>(), genres)) out.push_back(it);
  }
  return out;
}

std::vector<std::vector<std::string>> genre_sets() {
  return {{"Documentary"},          {"Western"},       {"Drama"},  {"Sci-Fi", "Romance"}, {"Animation", "Horror", "War"},
          {"Comedy", "Crime"},      {"Fantasy"},       {"Action"}, kAllGenres};
}

}  // namespace

// ---- validation -----------------------------------------------------------------------------------

TEST_CASE("recs-model: genres: invalid values are 400 bad_request", "[model-service][validation][genres]") {
  Fixture f;
  std::vector<json> bad = {
      json::array({"Sci-fi"}),               // case matters
      json::array({"(no genres listed)"}),   // not in the enum
      json::array({"Drama", "Nope"}),
      json::array({""}),
      json::array({"Drama", "Drama"}),       // duplicate
      json::array({"Action", "Comedy", "Action"}),
      json("Drama"),                         // wrong types
      json(3),
      json(nullptr),
      json(true),
      json::object(),
      json::array({3}),
      json::array({nullptr}),
      json::array({json::array({"Drama"})}),
      json::array({json::object({{"genre", "Drama"}})}),
  };
  // More than 19 items (necessarily with a duplicate, since the enum has 19 names).
  json twenty = json(kAllGenres);
  twenty.push_back("Drama");
  bad.push_back(twenty);
  json many = json::array();
  for (int i = 0; i < 40; ++i) many.push_back("Drama");
  bad.push_back(many);

  for (const json& g : bad) {
    INFO(g.dump());
    json body = rec_body("pop", 5, nullptr);
    body["genres"] = g;  // set directly: rec_body treats null as "absent"
    const auto r = f.post("/v1/recommend", body);
    CHECK(r.status == 400);
    CHECK(code_of(r) == "bad_request");
    json cmp = {{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 5}, {"genres", g}};
    const auto c = f.post("/v1/compare", cmp);
    CHECK(c.status == 400);
    CHECK(code_of(c) == "bad_request");
  }
}

TEST_CASE("recs-model: genres: every enum value is accepted, all 19 at once too", "[model-service][validation][genres]") {
  Fixture f;
  for (const auto& g : kAllGenres) {
    INFO(g);
    CHECK(f.post("/v1/recommend", rec_body("pop", 5, json::array({g}))).status == 200);
  }
  CHECK(f.post("/v1/recommend", rec_body("pop", 5, json(kAllGenres))).status == 200);
}

TEST_CASE("recs-model: genres: absent and empty mean no filter", "[model-service][genres]") {
  Fixture f;
  for (const auto& m : kModels) {
    INFO(m);
    const json absent = items_of(f, rec_body(m, 100, nullptr));
    const json empty = items_of(f, rec_body(m, 100, json::array()));
    CHECK(absent == empty);
    // Unfiltered, the movie with no genres at all is still a candidate (only the two rated are masked).
    CHECK(absent.size() == recs::testing::fixture_catalog_rows().size() - 2);
  }
}

// ---- behaviour ------------------------------------------------------------------------------------

TEST_CASE("recs-model: genres: filtered top-k is the filtered unfiltered ranking, for every model", "[model-service][genres]") {
  Fixture f;
  for (const auto& m : kModels) {
    const json full = items_of(f, rec_body(m, 100, nullptr));  // the catalog is smaller than 100
    for (const auto& genres : genre_sets()) {
      for (std::size_t k : {1u, 3u, 5u, 100u}) {
        INFO(m << " k=" << k << " genres=" << json(genres).dump());
        const json got = items_of(f, rec_body(m, k, json(genres)));
        for (const auto& it : got) CHECK(matches(it["movie_id"].get<std::int64_t>(), genres));
        CHECK(got == expected_filtered(full, genres, k));
      }
    }
  }
}

TEST_CASE("recs-model: genres: low-ranked genres still return their movies", "[model-service][genres]") {
  Fixture f;
  // pop_count ranks by rating count: the only Documentary (7003, 1 rating) and the only Western
  // (1081, 5 ratings) are far below the top 3 unfiltered.
  const json top3 = items_of(f, rec_body("pop_count", 3, nullptr));
  for (const auto& it : top3) {
    CHECK(it["movie_id"] != 7003);
    CHECK(it["movie_id"] != 1081);
  }
  const json doc = items_of(f, rec_body("pop_count", 3, json::array({"Documentary"})));
  REQUIRE(doc.size() == 1);
  CHECK(doc[0]["movie_id"] == 7003);
  const json west = items_of(f, rec_body("pop_count", 3, json::array({"Western", "Documentary"})));
  REQUIRE(west.size() == 2);
  CHECK(west[0]["movie_id"] == 1081);  // 5 ratings beat 1
  CHECK(west[1]["movie_id"] == 7003);
}

TEST_CASE("recs-model: genres: fewer than k when few match; a genre with no movies is empty", "[model-service][genres]") {
  Fixture f;
  for (const auto& m : kModels) {
    INFO(m);
    const json west = items_of(f, rec_body(m, 10, json::array({"Western"})));
    REQUIRE(west.size() == 1);
    CHECK(west[0]["movie_id"] == 1081);
    for (const char* none : {"IMAX", "Film-Noir", "Musical"}) {
      INFO(none);
      const auto r = f.post("/v1/recommend", rec_body(m, 10, json::array({none})));
      REQUIRE(r.status == 200);
      CHECK(json::parse(r.body)["items"] == json::array());
    }
    // A movie without genres never matches, even with every genre listed.
    for (const auto& it : items_of(f, rec_body(m, 100, json(kAllGenres)))) CHECK(it["movie_id"] != 7002);
  }
}

TEST_CASE("recs-model: genres: exclude and rated movies are still masked", "[model-service][genres]") {
  Fixture f;
  for (const auto& m : kModels) {
    INFO(m);
    const std::vector<std::string> drama = {"Drama"};
    const json full = items_of(f, rec_body(m, 100, nullptr, json::array({318, 2959})));
    const json got = items_of(f, rec_body(m, 100, json(drama), json::array({318, 2959})));
    CHECK(got == expected_filtered(full, drama, 100));
    for (const auto& it : got) {
      CHECK(it["movie_id"] != 318);
      CHECK(it["movie_id"] != 2959);
    }
    // Rated movies (260, 1) stay masked under a filter that matches them.
    for (const auto& it : items_of(f, rec_body(m, 100, json::array({"Sci-Fi", "Animation"})))) {
      CHECK(it["movie_id"] != 260);
      CHECK(it["movie_id"] != 1);
    }
    // Excluding the only Western leaves nothing.
    CHECK(items_of(f, rec_body(m, 10, json::array({"Western"}), json::array({1081}))) == json::array());
  }
}

TEST_CASE("recs-model: genres: compare filters every model the same way recommend does", "[model-service][genres]") {
  Fixture f;
  for (const auto& genres : genre_sets()) {
    for (std::size_t k : {2u, 10u}) {
      INFO("k=" << k << " genres=" << json(genres).dump());
      const json body = {{"models", {"pop", "pop_count", "knn", "mf", "als"}},
                         {"ratings", some_ratings()},
                         {"k", k},
                         {"exclude", {356}},
                         {"genres", genres}};
      const auto r = f.post("/v1/compare", body);
      REQUIRE(r.status == 200);
      const json b = json::parse(r.body);
      REQUIRE(recs::testing::validate_against(b, "CompareResponse").empty());
      REQUIRE(b["results"].size() == 5);
      for (const auto& res : b["results"]) {
        const std::string m = res["model"];
        INFO(m);
        const json full = items_of(f, rec_body(m, 100, nullptr, json::array({356})));
        CHECK(res["items"] == expected_filtered(full, genres, k));
        CHECK(res["items"] == items_of(f, rec_body(m, k, json(genres), json::array({356}))));
      }
    }
  }
  // Empty list on compare means no filter.
  json body = {{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 100}};
  const auto a = f.post("/v1/compare", body);
  body["genres"] = json::array();
  const auto e = f.post("/v1/compare", body);
  REQUIRE(a.status == 200);
  CHECK(a.body == e.body);
}

TEST_CASE("recs-model: genres: request schemas accept and reject like the service", "[model-service][contract][genres]") {
  // The transcribed request schemas (schema.hpp) agree with the service on genres.
  using recs::testing::validate_against;
  CHECK(validate_against(rec_body("pop", 5, json::array({"Drama", "War"})), "RecommendRequest").empty());
  CHECK(validate_against(rec_body("pop", 5, json::array()), "RecommendRequest").empty());
  CHECK(validate_against(rec_body("pop", 5, json(kAllGenres)), "RecommendRequest").empty());
  CHECK_FALSE(validate_against(rec_body("pop", 5, json::array({"Drama", "Drama"})), "RecommendRequest").empty());
  CHECK_FALSE(validate_against(rec_body("pop", 5, json::array({"Sci-fi"})), "RecommendRequest").empty());
  CHECK_FALSE(validate_against(rec_body("pop", 5, json("Drama")), "RecommendRequest").empty());
  json cmp = {{"models", {"pop", "knn"}}, {"ratings", some_ratings()}, {"k", 5}, {"genres", {"Western"}}};
  CHECK(validate_against(cmp, "CompareRequest").empty());
  cmp["genres"] = {"Western", "Western"};
  CHECK_FALSE(validate_against(cmp, "CompareRequest").empty());
}

TEST_CASE("recs-model: genres: catalog genres outside the enum are ignored, not matched", "[model-service][genres]") {
  // Regression: a catalog genre string the contract enum does not know ("Superhero") gets no bit.
  // 1081 has it plus a known genre (Western) and matches only via Western; 7003 has only the
  // unknown genre and so matches no filter. Bundle load must not fail on either.
  auto artifact = recs::testing::fixture_artifact();
  for (auto& m : artifact.catalog.movies) {
    if (m.movie_id == 1081) m.genres = {"Superhero", "Western"};
    if (m.movie_id == 7003) m.genres = {"Superhero"};
  }
  recs::service::BundleHolder holder;
  REQUIRE_NOTHROW(holder.store(recs::service::make_bundle(std::move(artifact))));
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  const auto items = [&](const json& body) {
    const auto r = svc.handle({"POST", "/v1/recommend", {}, "", body.dump()});
    INFO(body.dump() << " -> " << r.body);
    REQUIRE(r.status == 200);
    return json::parse(r.body)["items"];
  };

  for (const auto& m : kModels) {
    INFO(m);
    const json west = items(rec_body(m, 10, json::array({"Western"})));
    REQUIRE(west.size() == 1);
    CHECK(west[0]["movie_id"] == 1081);
    // 7003 was the only Documentary; with only an unknown genre left, nothing matches.
    CHECK(items(rec_body(m, 10, json::array({"Documentary"}))) == json::array());
    for (const auto& it : items(rec_body(m, 100, json(kAllGenres)))) CHECK(it["movie_id"] != 7003);
    // Unfiltered, 7003 is still an ordinary candidate.
    bool found = false;
    for (const auto& it : items(rec_body(m, 100, nullptr))) found = found || it["movie_id"] == 7003;
    CHECK(found);
  }
  // The unknown name is still rejected in a request.
  const auto bad = svc.handle({"POST", "/v1/recommend", {}, "", rec_body("pop", 5, json::array({"Superhero"})).dump()});
  CHECK(bad.status == 400);
  CHECK(code_of(bad) == "bad_request");
}
