// Gate tests for the RFC 4180 CSV reader (spec Phase 0 gate 1). Every fixture in
// tests/fixtures/csv/ must behave exactly as tests/fixtures/csv/README.md says.
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "recs/csv.hpp"

namespace {

using Records = std::vector<std::vector<std::string>>;

std::string read_fixture(const std::string& name) {
  std::ifstream in(recs::testing::csv_fixtures_dir() / name, std::ios::binary);
  REQUIRE(in);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Reads a fixture through the streaming CsvReader (the path used by the ingest code).
Records read_all(const std::string& name, std::optional<std::size_t> expected = 3) {
  std::ifstream in(recs::testing::csv_fixtures_dir() / name, std::ios::binary);
  REQUIRE(in);
  recs::CsvReader reader(in, name, expected);
  Records out;
  std::vector<std::string> fields;
  while (reader.next(fields)) {
    CHECK(reader.row() == out.size() + 1);
    out.push_back(fields);
  }
  return out;
}

// Returns the row of the CsvError thrown while reading the fixture, checking the source name too.
std::size_t error_row(const std::string& name, std::optional<std::size_t> expected = 3) {
  try {
    read_all(name, expected);
  } catch (const recs::CsvError& e) {
    CHECK(e.source() == name);
    CHECK(std::string(e.what()).starts_with(name + ":" + std::to_string(e.row()) + ": "));
    return e.row();
  }
  FAIL("expected CsvError from " << name);
  return 0;
}

const std::vector<std::string> kHeader = {"movieId", "title", "genres"};

}  // namespace

TEST_CASE("csv fixture: quoted_comma", "[csv]") {
  const auto r = read_all("quoted_comma.csv");
  REQUIRE(r.size() == 2);
  CHECK(r[0] == kHeader);
  CHECK(r[1] == std::vector<std::string>{"11", "American President, The (1995)", "Comedy|Drama|Romance"});
}

TEST_CASE("csv fixture: escaped_quote", "[csv]") {
  const auto r = read_all("escaped_quote.csv");
  REQUIRE(r.size() == 2);
  CHECK(r[1] == std::vector<std::string>{"26", "\"Great Performances\" Cats (1998)", "Musical"});
}

TEST_CASE("csv fixture: bom is skipped", "[csv]") {
  const auto r = read_all("bom.csv");
  REQUIRE(r.size() == 2);
  CHECK(r[0] == kHeader);
  CHECK(r[0][0].size() == 7);
  CHECK(r[1] == std::vector<std::string>{"1", "Toy Story (1995)", "Adventure"});
}

TEST_CASE("csv fixture: crlf", "[csv]") {
  const auto r = read_all("crlf.csv");
  REQUIRE(r.size() == 3);
  for (const auto& rec : r) {
    for (const auto& f : rec) CHECK((f.empty() || f.back() != '\r'));
  }
  CHECK(r[0] == kHeader);
  CHECK(r[1] == std::vector<std::string>{"1", "Toy Story (1995)", "Adventure"});
  CHECK(r[2] == std::vector<std::string>{"2", "Jumanji, The (1995)", "Fantasy"});
}

TEST_CASE("csv fixture: no_trailing_newline", "[csv]") {
  SECTION("with expected_fields = 3 the short final record fails at row 3") {
    CHECK(error_row("no_trailing_newline.csv", 3) == 3);
  }
  SECTION("without an expected count the final record parses as 2 fields") {
    const auto r = read_all("no_trailing_newline.csv", std::nullopt);
    REQUIRE(r.size() == 3);
    CHECK(r[1] == std::vector<std::string>{"1", "Toy Story (1995)", "Adventure"});
    CHECK(r[2] == std::vector<std::string>{"2", "Jumanji (1995)"});
  }
}

TEST_CASE("csv fixture: embedded_newline", "[csv]") {
  const auto r = read_all("embedded_newline.csv");
  REQUIRE(r.size() == 2);
  CHECK(r[1] == std::vector<std::string>{"1", "Multi\nLine", "Drama"});
}

TEST_CASE("csv fixture: malformed files throw CsvError at the documented row", "[csv]") {
  CHECK(error_row("malformed_unterminated.csv") == 3);
  CHECK(error_row("malformed_field_count.csv") == 4);
  CHECK(error_row("malformed_stray_quote.csv") == 2);
  CHECK(error_row("malformed_after_quote.csv") == 2);
}

TEST_CASE("csv fixture: malformed files fail the same way without an expected field count", "[csv]") {
  CHECK(error_row("malformed_unterminated.csv", std::nullopt) == 3);
  CHECK(error_row("malformed_stray_quote.csv", std::nullopt) == 2);
  CHECK(error_row("malformed_after_quote.csv", std::nullopt) == 2);
}

TEST_CASE("parse_csv agrees with CsvReader on every fixture", "[csv]") {
  for (const char* name : {"quoted_comma.csv", "escaped_quote.csv", "bom.csv", "crlf.csv",
                           "no_trailing_newline.csv", "embedded_newline.csv"}) {
    INFO(name);
    CHECK(recs::parse_csv(read_fixture(name), name) == read_all(name, std::nullopt));
  }
}

TEST_CASE("csv inline cases", "[csv]") {
  SECTION("empty unquoted field") {
    CHECK(recs::parse_csv("a,,c\n") == Records{{"a", "", "c"}});
  }
  SECTION("empty quoted field") {
    CHECK(recs::parse_csv("\"\"\n") == Records{{""}});
    CHECK(recs::parse_csv("a,\"\",c") == Records{{"a", "", "c"}});
  }
  SECTION("trailing empty field") {
    CHECK(recs::parse_csv("a,b,\n") == Records{{"a", "b", ""}});
  }
  SECTION("a lone CR inside quotes is literal") {
    CHECK(recs::parse_csv("\"x\ry\",b\r\n") == Records{{"x\ry", "b"}});
  }
  SECTION("CRLF inside quotes is literal") {
    CHECK(recs::parse_csv("\"x\r\ny\",b\n") == Records{{"x\r\ny", "b"}});
  }
  SECTION("a field that is only four quotes is one quote") {
    CHECK(recs::parse_csv("\"\"\"\"\n") == Records{{"\""}});
    CHECK(recs::parse_csv("a,\"\"\"\",c") == Records{{"a", "\"", "c"}});
  }
  SECTION("empty input has no records") {
    CHECK(recs::parse_csv("").empty());
    CHECK(recs::parse_csv("\xEF\xBB\xBF").empty());
  }
  SECTION("a BOM is only skipped at the very start") {
    const auto r = recs::parse_csv("a\n\xEF\xBB\xBF" "b\n");
    REQUIRE(r.size() == 2);
    CHECK(r[1][0] == "\xEF\xBB\xBF" "b");
  }
  SECTION("UTF-8 passes through untouched") {
    CHECK(recs::parse_csv("18,\"Am\xC3\xA9lie\"\n") == Records{{"18", "Am\xC3\xA9lie"}});
  }
  SECTION("row numbers count records, not lines") {
    try {
      recs::parse_csv("h\n\"a\nb\nc\"\nx\"y\n", "src");
      FAIL("expected CsvError");
    } catch (const recs::CsvError& e) {
      CHECK(e.row() == 3);
      CHECK(e.source() == "src");
    }
  }
  SECTION("characters after a closing quote, including a lone CR") {
    CHECK_THROWS_AS(recs::parse_csv("\"a\"b\n"), recs::CsvError);
    CHECK_THROWS_AS(recs::parse_csv("\"a\"\rb\n"), recs::CsvError);
  }
  SECTION("unterminated quote at end of input") {
    CHECK_THROWS_AS(recs::parse_csv("\"abc"), recs::CsvError);
  }
  SECTION("expected field count applies to every record") {
    CHECK(recs::parse_csv("a,b\nc,d\n", "<s>", 2).size() == 2);
    CHECK_THROWS_AS(recs::parse_csv("a,b\nc\n", "<s>", 2), recs::CsvError);
    CHECK_THROWS_AS(recs::parse_csv("a,b,c\n", "<s>", 2), recs::CsvError);
  }
  SECTION("a blank line is an empty record with a clear error when fields are expected") {
    CHECK(recs::parse_csv("a\n\nb\n") == Records{{"a"}, {""}, {"b"}});
    try {
      recs::parse_csv("a,b\n\nc,d\n", "<s>", 2);
      FAIL("expected CsvError");
    } catch (const recs::CsvError& e) {
      CHECK(e.row() == 2);
      CHECK(std::string(e.what()).find("empty record") != std::string::npos);
    }
  }
}

TEST_CASE("CsvReader reuses the caller's field vector across records", "[csv]") {
  std::istringstream in("a,b,c\nd\ne,f\n");
  recs::CsvReader reader(in, "s");
  std::vector<std::string> fields;
  REQUIRE(reader.next(fields));
  CHECK(fields == std::vector<std::string>{"a", "b", "c"});
  REQUIRE(reader.next(fields));
  CHECK(fields == std::vector<std::string>{"d"});
  REQUIRE(reader.next(fields));
  CHECK(fields == std::vector<std::string>{"e", "f"});
  CHECK(reader.row() == 3);
  CHECK_FALSE(reader.next(fields));
  CHECK_FALSE(reader.next(fields));
}
