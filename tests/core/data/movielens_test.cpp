// Gate tests for MovieLens ingest (spec Phase 0 gates 2 and 3) on the hand-checkable `tiny`
// fixture, small temporary CSV directories for the failure modes, and ml-latest-small / ml-32m
// when they are present under data/raw/.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fixtures.hpp"
#include "recs/csv.hpp"
#include "recs/dataset.hpp"
#include "recs/sparse.hpp"
#include "dataset_eq.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using recs::testing::TempDir;

namespace {

const recs::MovieMeta& movie(const recs::Dataset& ds, std::int64_t movie_id) {
  const auto idx = ds.items.find(movie_id);
  REQUIRE(idx.has_value());
  return ds.movies.at(*idx);
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<std::string> split(const std::string& line, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

// The raw data directory: RECS_DATA_DIR (the env var) overrides the build-time default.
fs::path raw_data_dir() {
  if (const char* env = std::getenv("RECS_DATA_DIR"); env != nullptr && *env != '\0') return fs::path(env) / "raw";
  return recs::testing::data_dir() / "raw";
}

// Writes a minimal MovieLens directory: tiny's movies.csv plus the given ratings rows.
void write_small_dir(const TempDir& dir, const std::string& rating_rows) {
  dir.write("movies.csv", slurp(recs::testing::tiny_dir() / "movies.csv"));
  dir.write("ratings.csv", "userId,movieId,rating,timestamp\n" + rating_rows);
}

// Asserts that loading `dir` throws an error naming `file` and `row`.
void require_load_error(const TempDir& dir, const std::string& file, std::size_t row) {
  try {
    (void)recs::load_movielens(dir.path(), "bad");
  } catch (const recs::DatasetError& e) {
    INFO(e.what());
    CHECK_THAT(e.what(), ContainsSubstring(file + ":" + std::to_string(row) + ":"));
    return;
  } catch (const recs::CsvError& e) {
    INFO(e.what());
    CHECK_THAT(e.source(), ContainsSubstring(file));
    CHECK(e.row() == row);
    return;
  }
  FAIL("expected an ingest error at " << file << ":" << row);
}

}  // namespace

TEST_CASE("tiny: counts", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  CHECK(ds.name == "tiny");
  CHECK(ds.n_ratings() == 245);
  CHECK(ds.n_items() == 30);
  CHECK(ds.n_users() == 20);
  CHECK(ds.tags.size() == 11);
  CHECK(ds.movies.size() == 30);
  CHECK_FALSE(ds.genome.has_value());
}

TEST_CASE("tiny: movie metadata", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");

  const auto& m1 = movie(ds, 1);
  CHECK(m1.movie_id == 1);
  CHECK(m1.title == "Toy Story");
  CHECK(m1.year == 1995);
  CHECK(m1.genres == std::vector<std::string>{"Adventure", "Animation", "Children", "Comedy", "Fantasy"});
  CHECK(m1.imdb_id == 114709);
  CHECK(m1.tmdb_id == 862);

  const auto& m4 = movie(ds, 4);
  CHECK(m4.title == "American President, The");
  CHECK(m4.year == 1995);

  const auto& m5 = movie(ds, 5);
  CHECK(m5.title == "City of Lost Children, The (Cit\xC3\xA9 des enfants perdus, La)");
  CHECK(m5.year == 1995);

  const auto& m26 = movie(ds, 26);
  CHECK(m26.title == "\"Great Performances\" Cats");
  CHECK(m26.year == 1998);
  CHECK(m26.imdb_id == 173714);
  CHECK_FALSE(m26.tmdb_id.has_value());

  const auto& m27 = movie(ds, 27);
  CHECK(m27.title == "Hamlet, Prince of Denmark");
  CHECK_FALSE(m27.year.has_value());
  CHECK_FALSE(m27.imdb_id.has_value());
  CHECK_FALSE(m27.tmdb_id.has_value());

  const auto& m28 = movie(ds, 28);
  CHECK(m28.genres.empty());
  CHECK(m28.year == 2003);
}

TEST_CASE("tiny: index order and the cold item", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");

  // Items follow movies.csv order.
  REQUIRE(ds.items.size() == 30);
  for (std::uint32_t i = 0; i < 30; ++i) {
    CHECK(ds.items.to_external[i] == i + 1);
    CHECK(ds.items.find(i + 1) == i);
    CHECK(ds.movies[i].movie_id == i + 1);
  }

  // Users follow order of first appearance in ratings.csv (computed independently here).
  std::vector<std::int64_t> first_seen;
  std::set<std::int64_t> seen;
  std::istringstream lines(slurp(recs::testing::tiny_dir() / "ratings.csv"));
  std::string line;
  std::getline(lines, line);  // header
  while (std::getline(lines, line)) {
    const auto u = std::stoll(split(line, ',')[0]);
    if (seen.insert(u).second) first_seen.push_back(u);
  }
  CHECK(ds.users.to_external == first_seen);
  for (std::uint32_t u = 0; u < ds.n_users(); ++u) CHECK(ds.users.find(ds.users.to_external[u]) == u);

  // Movie 30 has no ratings but still has an ItemIdx; movie 29 is rated only by user 7.
  const auto i30 = ds.items.find(30);
  REQUIRE(i30.has_value());
  CHECK(ds.by_item.row_size(*i30) == 0);
  const auto i29 = ds.items.find(29);
  REQUIRE(i29.has_value());
  REQUIRE(ds.by_item.row_size(*i29) == 1);
  CHECK(ds.users.to_external[ds.by_item.row_indices(*i29)[0]] == 7);
  CHECK(ds.by_item.row_values(*i29)[0] == 4.5f);
}

TEST_CASE("tiny: every rating in ratings.csv is in the matrix", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  std::istringstream lines(slurp(recs::testing::tiny_dir() / "ratings.csv"));
  std::string line;
  std::getline(lines, line);
  std::size_t n = 0;
  while (std::getline(lines, line)) {
    const auto f = split(line, ',');
    const auto u = ds.users.find(std::stoll(f[0]));
    const auto i = ds.items.find(std::stoll(f[1]));
    REQUIRE(u.has_value());
    REQUIRE(i.has_value());
    const auto cols = ds.by_user.row_indices(*u);
    bool found = false;
    for (std::size_t k = 0; k < cols.size(); ++k) {
      if (cols[k] != *i) continue;
      found = true;
      CHECK(ds.by_user.row_values(*u)[k] == std::stof(f[2]));
      CHECK(ds.by_user.row_ts(*u)[k] == std::stoll(f[3]));
    }
    CHECK(found);
    ++n;
  }
  CHECK(n == ds.n_ratings());
}

TEST_CASE("tiny: by_user and by_item are valid and transposes of each other", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  REQUIRE_NOTHROW(recs::validate(ds.by_user));
  REQUIRE_NOTHROW(recs::validate(ds.by_item));
  CHECK(ds.by_user.n_rows == ds.n_users());
  CHECK(ds.by_user.n_cols == ds.n_items());
  CHECK(ds.by_item == recs::transpose(ds.by_user));
}

TEST_CASE("tiny: tags keep the external user id", "[ingest][tiny]") {
  const auto ds = recs::load_movielens(recs::testing::tiny_dir(), "tiny");
  REQUIRE(ds.tags.size() == 11);
  CHECK(ds.tags[0] == recs::TagApplication{1, *ds.items.find(1), "pixar", 1600000001});
  CHECK(ds.tags[7] == recs::TagApplication{6, *ds.items.find(10), "quotable, violent", 1600000008});
  CHECK(ds.tags[10] == recs::TagApplication{99, *ds.items.find(22), "creepy", 1600000011});
  CHECK_FALSE(ds.users.find(99).has_value());  // tagging user 99 never rated anything
}

TEST_CASE("ingest rejects bad ratings with file and row", "[ingest]") {
  SECTION("rating for a movie missing from movies.csv") {
    TempDir dir("ml-unknown");
    write_small_dir(dir, "1,1,4.0,100\n1,999,4.0,101\n");
    require_load_error(dir, "ratings.csv", 3);
  }
  for (const char* bad : {"0.0", "5.5", "3.25", "-1.0", "nan", "inf", "", "4.0x", "abc"}) {
    SECTION(std::string("rating value '") + bad + "'") {
      TempDir dir("ml-value");
      write_small_dir(dir, std::string("1,1,4.0,100\n1,2,") + bad + ",101\n");
      require_load_error(dir, "ratings.csv", 3);
    }
  }
  SECTION("duplicate (user, movie), not adjacent") {
    TempDir dir("ml-dup");
    write_small_dir(dir, "1,2,3.0,100\n2,1,4.0,1\n1,3,4.0,2\n1,2,5.0,200\n");
    require_load_error(dir, "ratings.csv", 5);
  }
  SECTION("non-numeric ids and timestamps") {
    TempDir dir("ml-ids");
    write_small_dir(dir, "1,1,4.0,100\nx,2,4.0,101\n");
    require_load_error(dir, "ratings.csv", 3);
    write_small_dir(dir, "1,1,4.0,100\n1,2,4.0,10 1\n");
    require_load_error(dir, "ratings.csv", 3);
  }
  SECTION("wrong field count") {
    TempDir dir("ml-fields");
    write_small_dir(dir, "1,1,4.0,100\n1,2,4.0\n");
    require_load_error(dir, "ratings.csv", 3);
  }
  SECTION("wrong header") {
    TempDir dir("ml-header");
    dir.write("movies.csv", slurp(recs::testing::tiny_dir() / "movies.csv"));
    dir.write("ratings.csv", "userId,movieId,score,timestamp\n1,1,4.0,100\n");
    require_load_error(dir, "ratings.csv", 1);
  }
  SECTION("boundary values 0.5 and 5.0 are accepted") {
    TempDir dir("ml-ok");
    write_small_dir(dir, "1,1,0.5,100\n1,2,5.0,101\n2,1,3,5\n");
    const auto ds = recs::load_movielens(dir.path(), "ok");
    CHECK(ds.n_ratings() == 3);
    CHECK(ds.n_users() == 2);
    CHECK(ds.n_items() == 30);
    CHECK(ds.tags.empty());
    for (const auto& m : ds.movies) {  // no links.csv: ids stay empty
      CHECK_FALSE(m.imdb_id.has_value());
      CHECK_FALSE(m.tmdb_id.has_value());
    }
  }
}

TEST_CASE("ingest rejects bad movies, links and tags with file and row", "[ingest]") {
  SECTION("missing required files") {
    TempDir dir("ml-missing");
    CHECK_THROWS_AS(recs::load_movielens(dir.path(), "x"), recs::DatasetError);
    dir.write("movies.csv", slurp(recs::testing::tiny_dir() / "movies.csv"));
    CHECK_THROWS_AS(recs::load_movielens(dir.path(), "x"), recs::DatasetError);
  }
  SECTION("duplicate movieId in movies.csv") {
    TempDir dir("ml-dupmovie");
    dir.write("movies.csv", "movieId,title,genres\n1,A (2000),Drama\n2,B,Drama\n1,C,Drama\n");
    dir.write("ratings.csv", "userId,movieId,rating,timestamp\n1,1,4.0,100\n");
    require_load_error(dir, "movies.csv", 4);
  }
  SECTION("tag for an unknown movie") {
    TempDir dir("ml-tag");
    write_small_dir(dir, "1,1,4.0,100\n");
    dir.write("tags.csv", "userId,movieId,tag,timestamp\n1,1,ok,5\n1,31,bad,6\n");
    require_load_error(dir, "tags.csv", 3);
  }
  SECTION("link for an unknown movie") {
    TempDir dir("ml-link");
    write_small_dir(dir, "1,1,4.0,100\n");
    dir.write("links.csv", "movieId,imdbId,tmdbId\n1,0114709,862\n77,1,2\n");
    require_load_error(dir, "links.csv", 3);
  }
  SECTION("malformed CSV in movies.csv reports the row") {
    TempDir dir("ml-csv");
    dir.write("movies.csv", "movieId,title,genres\n1,A,Drama\n2,\"B,Drama\n");
    dir.write("ratings.csv", "userId,movieId,rating,timestamp\n1,1,4.0,100\n");
    require_load_error(dir, "movies.csv", 3);
  }
}

TEST_CASE("title and year parsing", "[ingest]") {
  TempDir dir("ml-titles");
  dir.write("movies.csv",
            "movieId,title,genres\n"
            "1,From Dusk Till Dawn 2: Texas Blood Money (1999) ,Horror\n"
            "2,Babylon 5,Sci-Fi\n"
            "3,Some Series (1994-1998),Drama\n"
            "4,  Padded   (2001)  ,Drama\n"
            "5,Odd (199),Drama\n"
            "6,Year Inside (1999) Later,Drama\n"
            "7,(2010),Drama\n");
  dir.write("ratings.csv", "userId,movieId,rating,timestamp\n1,1,4.0,100\n");
  const auto ds = recs::load_movielens(dir.path(), "titles");
  CHECK(ds.movies[0].title == "From Dusk Till Dawn 2: Texas Blood Money");
  CHECK(ds.movies[0].year == 1999);
  CHECK(ds.movies[1].title == "Babylon 5");
  CHECK_FALSE(ds.movies[1].year.has_value());
  CHECK(ds.movies[2].title == "Some Series (1994-1998)");
  CHECK_FALSE(ds.movies[2].year.has_value());
  CHECK(ds.movies[3].title == "Padded");
  CHECK(ds.movies[3].year == 2001);
  CHECK(ds.movies[4].title == "Odd (199)");
  CHECK_FALSE(ds.movies[4].year.has_value());
  CHECK(ds.movies[5].title == "Year Inside (1999) Later");
  CHECK_FALSE(ds.movies[5].year.has_value());
  CHECK(ds.movies[6].title.empty());
  CHECK(ds.movies[6].year == 2010);
}

TEST_CASE("tag genome is loaded when both genome files are present", "[ingest]") {
  TempDir dir("ml-genome");
  write_small_dir(dir, "1,1,4.0,100\n");
  dir.write("genome-tags.csv", "tagId,tag\n1,funny\n2,\"dark, gritty\"\n");
  dir.write("genome-scores.csv", "movieId,tagId,relevance\n1,1,0.25\n1,2,0.75\n2,2,0.5\n");
  const auto ds = recs::load_movielens(dir.path(), "g");
  REQUIRE(ds.genome.has_value());
  const auto& g = *ds.genome;
  CHECK(g.n_items == 30);
  CHECK(g.n_tags == 2);
  CHECK(g.tag_names == std::vector<std::string>{"funny", "dark, gritty"});
  REQUIRE(g.relevance.size() == 60);
  CHECK(g.relevance[0 * 2 + 0] == 0.25f);
  CHECK(g.relevance[0 * 2 + 1] == 0.75f);
  CHECK(g.relevance[1 * 2 + 0] == 0.0f);  // no score -> 0
  CHECK(g.relevance[1 * 2 + 1] == 0.5f);

  SECTION("a score for an unknown tag is an error") {
    dir.write("genome-scores.csv", "movieId,tagId,relevance\n1,1,0.25\n1,3,0.5\n");
    require_load_error(dir, "genome-scores.csv", 3);
  }
  SECTION("a duplicate score is an error") {
    dir.write("genome-scores.csv", "movieId,tagId,relevance\n1,1,0.25\n1,1,0.5\n");
    require_load_error(dir, "genome-scores.csv", 3);
  }
  SECTION("only one genome file present is an error") {
    fs::remove(dir / "genome-tags.csv");
    CHECK_THROWS_AS(recs::load_movielens(dir.path(), "g"), recs::DatasetError);
  }
}

TEST_CASE("ml-latest-small: counts and known titles", "[ingest][ml-small]") {
  const auto raw = raw_data_dir() / "ml-latest-small";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-latest-small not present at " << raw);
  const auto ds = recs::load_movielens(raw, "ml-latest-small");
  CHECK(ds.n_ratings() == 100836);
  CHECK(ds.n_items() == 9742);
  CHECK(ds.n_users() == 610);
  CHECK(ds.tags.size() == 3683);
  REQUIRE_NOTHROW(recs::validate(ds.by_user));
  REQUIRE_NOTHROW(recs::validate(ds.by_item));
  CHECK(ds.by_item == recs::transpose(ds.by_user));

  bool dusk = false;
  bool babylon = false;
  for (const auto& m : ds.movies) {
    CHECK((m.title.empty() || (m.title.back() != ' ' && m.title.back() != '\r')));
    if (m.title == "From Dusk Till Dawn 2: Texas Blood Money") {
      dusk = true;
      CHECK(m.year == 1999);
    }
    if (m.title == "Babylon 5") {
      babylon = true;
      CHECK_FALSE(m.year.has_value());
    }
  }
  CHECK(dusk);
  CHECK(babylon);
}

TEST_CASE("ml-32m: counts, parse and cache timings", "[ingest][ml32m]") {
  const auto raw = raw_data_dir() / "ml-32m";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-32m not present at " << raw);
  using Clock = std::chrono::steady_clock;
  const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };

  const auto t0 = Clock::now();
  const auto ds = recs::load_movielens(raw, "ml-32m");
  const auto t1 = Clock::now();
  CHECK(ds.n_ratings() == 32000204);
  CHECK(ds.n_items() == 87585);
  CHECK(ds.n_users() == 200948);
  CHECK(ds.tags.size() == 2000072);
  REQUIRE_NOTHROW(recs::validate(ds.by_user));
  REQUIRE_NOTHROW(recs::validate(ds.by_item));

  TempDir dir("ml32m-cache");
  const auto cache = dir / "ml-32m.bin";
  const auto t2 = Clock::now();
  recs::save_cache(ds, cache);
  const auto t3 = Clock::now();
  const auto loaded = recs::load_cache(cache);
  const auto t4 = Clock::now();
  recs::testing::check_dataset_equal(loaded, ds);

  std::cout << "ml-32m: parse " << seconds(t1 - t0) << " s, cache save " << seconds(t3 - t2)
            << " s, cache load " << seconds(t4 - t3) << " s, cache size "
            << fs::file_size(cache) / (1024.0 * 1024.0) << " MiB\n";
}
