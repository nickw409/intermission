// Catalog search (case- and accent-insensitive, prefix first, then popularity) and onboarding
// (popular, genre-diverse) behaviour.
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "onboarding.hpp"
#include "search.hpp"
#include "test_bundle.hpp"

namespace {

using recs::service::normalize_text;

std::vector<std::int64_t> search_ids(const recs::service::Bundle& b, std::string_view q, std::size_t limit = 50) {
  std::vector<std::int64_t> out;
  for (auto i : b.search.search(q, limit)) out.push_back(b.catalog.items.to_external[i]);
  return out;
}

}  // namespace

TEST_CASE("normalize_text folds case, accents and punctuation", "[model-service][search]") {
  CHECK(normalize_text("Star Wars: Episode IV") == "star wars episode iv");
  CHECK(normalize_text("Amélie") == "amelie");
  CHECK(normalize_text("AME\xCC\x81LIE") == "amelie");  // decomposed: E + combining acute
  CHECK(normalize_text("ÀÉÎÕÜÇÑ") == "aeiouc" "n");
  CHECK(normalize_text("Straße") == "strasse");
  CHECK(normalize_text("Æon Œuvre Øre Łódź") == "aeon oeuvre ore lodz");
  CHECK(normalize_text("d'Amélie d\xE2\x80\x99" "Amélie") == "damelie damelie");
  CHECK(normalize_text("  Fight   Club!! ") == "fight club");
  CHECK(normalize_text("") == "");
  CHECK(normalize_text("\xff") == std::nullopt);
  CHECK(normalize_text("\xc3") == std::nullopt);
  CHECK(normalize_text("\xed\xa0\x80") == std::nullopt);  // UTF-16 surrogate
  CHECK(normalize_text("\xc0\xaf") == std::nullopt);      // overlong '/'
  CHECK(normalize_text("Кино") == "Кино");                // non-Latin text kept as is
}

TEST_CASE("article_first_title moves MovieLens trailing articles", "[model-service][search]") {
  using recs::service::article_first_title;
  CHECK(article_first_title("Matrix, The") == "The Matrix");
  CHECK(article_first_title("Lord of the Rings: The Fellowship of the Ring, The") ==
        "The Lord of the Rings: The Fellowship of the Ring");
  CHECK(article_first_title("Shawshank Redemption, The") == "The Shawshank Redemption");
  CHECK(article_first_title("City of Lost Children, The (Cité des enfants perdus, La)") ==
        "The City of Lost Children (Cité des enfants perdus, La)");
  CHECK(article_first_title("Amélie (Fabuleux destin d'Amélie Poulain, Le)") == std::nullopt);
  CHECK(article_first_title("Hamlet, Prince of Denmark") == std::nullopt);
  CHECK(article_first_title("Fight Club") == std::nullopt);
}

TEST_CASE("search: prefix matches first, then substring, each by popularity", "[model-service][search]") {
  const auto b = recs::testing::fixture_bundle();
  // Prefix "star": the three Star Wars films by rating count (251, 211, 196); then "Lone Star" (a
  // substring match).
  CHECK(search_ids(*b, "star") == std::vector<std::int64_t>{260, 1196, 1210, 1081});
  CHECK(search_ids(*b, "star", 3) == std::vector<std::int64_t>{260, 1196, 1210});
  CHECK(search_ids(*b, "STAR wars") == std::vector<std::int64_t>{260, 1196, 1210});
  CHECK(search_ids(*b, "star wars: episode v") == std::vector<std::int64_t>{1196, 1210});
  CHECK(search_ids(*b, "amelie") == std::vector<std::int64_t>{4973});
  CHECK(search_ids(*b, "AMÉLIE") == std::vector<std::int64_t>{4973});
  CHECK(search_ids(*b, "strasse") == std::vector<std::int64_t>{7003});
  CHECK(search_ids(*b, "the matrix") == std::vector<std::int64_t>{2571});
  CHECK(search_ids(*b, "matrix") == std::vector<std::int64_t>{2571});
  // "the": prefix via the article-first form, ordered by popularity: Shawshank (317), Silence (279),
  // Matrix (278), Lord of the Rings (198). Then the substring-only matches "... - The Empire
  // Strikes Back" (211) and "... Return of the Jedi" (196).
  CHECK(search_ids(*b, "the") == std::vector<std::int64_t>{318, 593, 2571, 4993, 1196, 1210});
  CHECK(search_ids(*b, "zzz").empty());
  CHECK(search_ids(*b, "   ").empty());
}

TEST_CASE("onboarding: popular, genre-diverse, never excluded or unrated", "[model-service][onboarding]") {
  const auto b = recs::testing::fixture_bundle();
  const auto ext = [&](const std::vector<recs::ItemIdx>& v) {
    std::vector<std::int64_t> out;
    for (auto i : v) out.push_back(b->catalog.items.to_external[i]);
    return out;
  };
  const auto idx = [&](std::int64_t id) { return *b->catalog.items.find(id); };

  const auto first = ext(recs::service::onboarding_picks(b->catalog, b->by_popularity, {}, 1));
  CHECK(first == std::vector<std::int64_t>{356});  // the most-rated movie

  std::unordered_set<recs::ItemIdx> excl{idx(260), idx(2571), idx(296), idx(1), idx(318)};
  const auto picks = recs::service::onboarding_picks(b->catalog, b->by_popularity, excl, 4);
  REQUIRE(picks.size() == 4);
  for (auto p : picks) CHECK_FALSE(excl.contains(p));
  CHECK(std::set<recs::ItemIdx>(picks.begin(), picks.end()).size() == 4);
  // Diversity: the first four picks share no genre with the most popular pick (Forrest Gump:
  // Comedy, Drama, Romance, War) except where unavoidable, so they cover more genres than the top-4
  // by popularity alone.
  std::set<std::string> genres_diverse;
  for (auto p : picks) genres_diverse.insert(b->catalog.movies[p].genres.begin(), b->catalog.movies[p].genres.end());
  std::set<std::string> genres_top;
  std::size_t taken = 0;
  for (auto i : b->by_popularity) {
    if (excl.contains(i) || taken == 4) continue;
    genres_top.insert(b->catalog.movies[i].genres.begin(), b->catalog.movies[i].genres.end());
    ++taken;
  }
  CHECK(genres_diverse.size() > genres_top.size());

  // Asking for more than exists returns every rated, non-excluded movie once; "Never Rated" is never offered.
  const auto all = recs::service::onboarding_picks(b->catalog, b->by_popularity, {}, 100);
  CHECK(all.size() == recs::testing::fixture_catalog_rows().size() - 1);
  for (auto p : all) CHECK(b->catalog.rating_count[p] > 0);
  // Deterministic.
  CHECK(recs::service::onboarding_picks(b->catalog, b->by_popularity, excl, 4) == picks);
}
