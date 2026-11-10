// MovieLens ingest: CSV files -> Dataset in dense index space.
//
// Files (https://files.grouplens.org/datasets/movielens/ml-32m-README.html):
//   movies.csv         movieId,title,genres          required; defines the item index order
//   ratings.csv        userId,movieId,rating,timestamp   required; defines the user index order
//   links.csv          movieId,imdbId,tmdbId         optional; either id may be empty
//   tags.csv           userId,movieId,tag,timestamp  optional
//   genome-tags.csv    tagId,tag                     optional, together with genome-scores.csv
//   genome-scores.csv  movieId,tagId,relevance       (ships with some releases, e.g. ml-25m)
//
// Index assignment: every movie in movies.csv gets the next ItemIdx in file order, so movies with
// no ratings still exist in the catalog. Users get the next UserIdx on their first appearance in
// ratings.csv. The rating matrix is built once as CSR (user -> items) with csr_from_triplets, and
// the CSC view (item -> users) is its transpose.
//
// Validation fails loudly with "<file>:<row>: <reason>" (rows are 1-based CSV records, the header
// is row 1): a wrong header, a non-numeric id, a rating outside [0.5, 5.0] or not a multiple of
// 0.5, a rating/tag/link/genome row for a movie that is not in movies.csv, a duplicate movieId,
// and a duplicate (user, movie) rating. Malformed CSV surfaces as CsvError with the same format.
//
// Titles: MovieLens appends the release year as "(YYYY)". After trimming whitespace, a title that
// ends in "(" + four digits + ")" has that year removed and the remaining text trimmed; anything
// else (no parentheses, a range such as "(1994-1998)", a three-digit year) keeps the title as is
// with no year. If removing the year would leave an empty title (a title that is only "(YYYY)"),
// the raw title is kept and the year is still recorded. Genres are '|'-separated; "(no genres
// listed)" becomes an empty list, and an empty genre token (as in "Action||Drama") is an error.
//
// Genome: relevance is stored densely as n_items x n_tags (rows indexed by ItemIdx, columns in
// genome-tags.csv order). Movies without genome scores keep relevance 0.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "recs/csv.hpp"
#include "recs/dataset.hpp"
#include "recs/sparse.hpp"

namespace recs {

namespace {

// An open CSV file with a large stream buffer (the default 8 KiB buffer is slow for ml-32m).
class CsvFile {
 public:
  CsvFile(const fs::path& path, std::size_t n_fields) : path_(path), buffer_(1 << 20) {
    in_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    in_.open(path, std::ios::binary);
    if (!in_) throw DatasetError(path.string() + ": cannot open file");
    reader_ = std::make_unique<CsvReader>(in_, path.string(), n_fields);
  }

  /// Reads the header and checks the column names exactly.
  void expect_header(const std::vector<std::string>& names) {
    std::vector<std::string> fields;
    if (!reader_->next(fields)) fail("empty file, expected a header");
    if (fields != names) {
      std::string want;
      for (const auto& n : names) want += (want.empty() ? "" : ",") + n;
      fail("unexpected header, expected '" + want + "'");
    }
  }

  bool next(std::vector<std::string>& fields) { return reader_->next(fields); }
  std::size_t row() const { return reader_->row(); }

  [[noreturn]] void fail(const std::string& what) const {
    throw DatasetError(path_.string() + ":" + std::to_string(row()) + ": " + what);
  }

 private:
  fs::path path_;
  std::vector<char> buffer_;
  std::ifstream in_;
  std::unique_ptr<CsvReader> reader_;
};

std::int64_t parse_int(const CsvFile& f, std::string_view s, const char* what) {
  std::int64_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
    f.fail(std::string("invalid ") + what + " '" + std::string(s) + "'");
  }
  return v;
}

std::optional<std::int64_t> parse_optional_int(const CsvFile& f, std::string_view s, const char* what) {
  if (s.empty()) return std::nullopt;
  return parse_int(f, s, what);
}

double parse_double(const CsvFile& f, std::string_view s, const char* what) {
  double v = 0.0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(v)) {
    f.fail(std::string("invalid ") + what + " '" + std::string(s) + "'");
  }
  return v;
}

/// A rating must lie in [0.5, 5.0] and be a multiple of 0.5.
float parse_rating(const CsvFile& f, std::string_view s) {
  const double v = parse_double(f, s, "rating");
  const double halves = v * 2.0;
  if (v < 0.5 || v > 5.0 || halves != std::floor(halves)) {
    f.fail("rating " + std::string(s) + " is not in {0.5, 1.0, ..., 5.0}");
  }
  return static_cast<float>(v);
}

std::string_view trim(std::string_view s) {
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

/// Splits "Title (YYYY)" into title and year (see the file comment for the exact rule).
void parse_title(std::string_view raw, MovieMeta& m) {
  const std::string_view t = trim(raw);
  constexpr std::size_t kSuffix = 6;  // "(YYYY)"
  if (t.size() >= kSuffix && t[t.size() - kSuffix] == '(' && t.back() == ')') {
    const std::string_view digits = t.substr(t.size() - 5, 4);
    if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      int year = 0;
      std::from_chars(digits.data(), digits.data() + digits.size(), year);
      m.year = year;
      const std::string_view stripped = trim(t.substr(0, t.size() - kSuffix));
      m.title = std::string(stripped.empty() ? t : stripped);  // never leave a movie without a title
      return;
    }
  }
  m.year.reset();
  m.title = std::string(t);
}

std::vector<std::string> parse_genres(const CsvFile& f, std::string_view s) {
  std::vector<std::string> out;
  if (s == "(no genres listed)" || s.empty()) return out;
  std::size_t start = 0;
  while (true) {
    const auto bar = s.find('|', start);
    const auto genre = s.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
    if (genre.empty()) f.fail("empty genre in '" + std::string(s) + "'");
    out.emplace_back(genre);
    if (bar == std::string_view::npos) break;
    start = bar + 1;
  }
  return out;
}

void load_movies(const fs::path& file, Dataset& ds) {
  CsvFile f(file, 3);
  f.expect_header({"movieId", "title", "genres"});
  std::vector<std::string> row;
  while (f.next(row)) {
    MovieMeta m;
    m.movie_id = parse_int(f, row[0], "movieId");
    const auto before = ds.items.size();
    if (ds.items.insert(m.movie_id) != before) f.fail("duplicate movieId " + row[0]);
    parse_title(row[1], m);
    m.genres = parse_genres(f, row[2]);
    ds.movies.push_back(std::move(m));
  }
}

void load_links(const fs::path& file, Dataset& ds) {
  CsvFile f(file, 3);
  f.expect_header({"movieId", "imdbId", "tmdbId"});
  std::vector<std::string> row;
  std::vector<bool> seen(ds.n_items(), false);
  while (f.next(row)) {
    const auto item = ds.items.find(parse_int(f, row[0], "movieId"));
    if (!item) f.fail("link for movieId " + row[0] + " which is not in movies.csv");
    if (seen[*item]) f.fail("duplicate link for movieId " + row[0]);
    seen[*item] = true;
    ds.movies[*item].imdb_id = parse_optional_int(f, row[1], "imdbId");
    ds.movies[*item].tmdb_id = parse_optional_int(f, row[2], "tmdbId");
  }
}

// Re-reads ratings.csv to name the row of the first repeated (user, movie) pair. Only called on
// the error path, after csr_from_triplets has reported that a duplicate exists.
[[noreturn]] void report_duplicate_rating(const fs::path& file, const Dataset& ds) {
  CsvFile f(file, 4);
  f.expect_header({"userId", "movieId", "rating", "timestamp"});
  std::vector<std::string> row;
  std::unordered_set<std::uint64_t> seen;
  while (f.next(row)) {
    const auto u = ds.users.find(parse_int(f, row[0], "userId"));
    const auto i = ds.items.find(parse_int(f, row[1], "movieId"));
    if (!u || !i) f.fail("unexpected row while locating a duplicate rating");
    const std::uint64_t key = (static_cast<std::uint64_t>(*u) << 32) | *i;
    if (!seen.insert(key).second) f.fail("duplicate rating for userId " + row[0] + ", movieId " + row[1]);
  }
  throw DatasetError(file.string() + ": duplicate rating reported but not found on re-read");
}

void load_ratings_impl(const fs::path& file, Dataset& ds) {
  std::vector<Triplet> triplets;
  {
    CsvFile f(file, 4);
    f.expect_header({"userId", "movieId", "rating", "timestamp"});
    // Pre-size for ~27 bytes per row (ml-32m), capped so a huge file cannot demand a huge
    // allocation up front; beyond the cap the vector simply grows.
    constexpr std::uintmax_t kMaxReserve = std::uintmax_t{1} << 26;  // 64M ratings
    std::error_code ec;
    const auto bytes = fs::file_size(file, ec);
    if (!ec) triplets.reserve(static_cast<std::size_t>(std::min(bytes / 24, kMaxReserve)));
    std::vector<std::string> row;
    std::int64_t last_user = 0;
    UserIdx last_idx = 0;
    bool have_last = false;
    while (f.next(row)) {
      const std::int64_t user = parse_int(f, row[0], "userId");
      const std::int64_t movie = parse_int(f, row[1], "movieId");
      const float value = parse_rating(f, row[2]);
      const std::int64_t ts = parse_int(f, row[3], "timestamp");
      const auto item = ds.items.find(movie);
      if (!item) f.fail("rating for movieId " + row[1] + " which is not in movies.csv");
      // ratings.csv is grouped by user in every MovieLens release; skip the hash lookup then.
      if (!have_last || user != last_user) {
        last_idx = ds.users.insert(user);
        last_user = user;
        have_last = true;
      }
      triplets.push_back({last_idx, *item, value, ts});
    }
  }
  try {
    ds.by_user = csr_from_triplets(ds.n_users(), ds.n_items(), std::move(triplets));
  } catch (const SparseError&) {
    report_duplicate_rating(file, ds);  // the only possible failure: ids are in range by construction
  }
  ds.by_item = transpose(ds.by_user);
}

void load_ratings(const fs::path& file, Dataset& ds) {
  try {
    load_ratings_impl(file, ds);
  } catch (const std::bad_alloc&) {
    throw DatasetError(file.string() + ": out of memory while loading ratings (" + std::to_string(ds.n_users()) +
                       " users so far)");
  } catch (const std::length_error&) {
    throw DatasetError(file.string() + ": too many ratings to hold in memory");
  }
}

void load_tags(const fs::path& file, Dataset& ds) {
  CsvFile f(file, 4);
  f.expect_header({"userId", "movieId", "tag", "timestamp"});
  std::vector<std::string> row;
  while (f.next(row)) {
    TagApplication t;
    t.user_id = parse_int(f, row[0], "userId");
    const auto item = ds.items.find(parse_int(f, row[1], "movieId"));
    if (!item) f.fail("tag for movieId " + row[1] + " which is not in movies.csv");
    t.item = *item;
    t.tag = std::move(row[2]);
    t.ts = parse_int(f, row[3], "timestamp");
    ds.tags.push_back(std::move(t));
  }
}

GenomeMatrix load_genome(const fs::path& tags_file, const fs::path& scores_file, const Dataset& ds) {
  GenomeMatrix g;
  std::unordered_map<std::int64_t, std::uint32_t> column;  // tagId -> column
  {
    CsvFile f(tags_file, 2);
    f.expect_header({"tagId", "tag"});
    std::vector<std::string> row;
    while (f.next(row)) {
      const auto id = parse_int(f, row[0], "tagId");
      if (!column.try_emplace(id, static_cast<std::uint32_t>(g.tag_names.size())).second) {
        f.fail("duplicate tagId " + row[0]);
      }
      g.tag_names.push_back(std::move(row[1]));
    }
  }
  g.n_items = ds.n_items();
  g.n_tags = static_cast<std::uint32_t>(g.tag_names.size());
  const std::size_t cells = static_cast<std::size_t>(g.n_items) * g.n_tags;
  g.relevance.assign(cells, 0.0f);
  std::vector<bool> seen(cells, false);

  CsvFile f(scores_file, 3);
  f.expect_header({"movieId", "tagId", "relevance"});
  std::vector<std::string> row;
  while (f.next(row)) {
    const auto item = ds.items.find(parse_int(f, row[0], "movieId"));
    if (!item) f.fail("genome score for movieId " + row[0] + " which is not in movies.csv");
    const auto col = column.find(parse_int(f, row[1], "tagId"));
    if (col == column.end()) f.fail("genome score for tagId " + row[1] + " which is not in genome-tags.csv");
    const double rel = parse_double(f, row[2], "relevance");
    if (rel < 0.0 || rel > 1.0) f.fail("relevance " + row[2] + " is not in [0, 1]");
    const std::size_t cell = static_cast<std::size_t>(*item) * g.n_tags + col->second;
    if (seen[cell]) f.fail("duplicate genome score for movieId " + row[0] + ", tagId " + row[1]);
    seen[cell] = true;
    g.relevance[cell] = static_cast<float>(rel);
  }
  return g;
}

}  // namespace

Dataset load_movielens(const fs::path& raw_dir, std::string name) {
  for (const char* required : {"movies.csv", "ratings.csv"}) {
    if (!fs::is_regular_file(raw_dir / required)) {
      throw DatasetError((raw_dir / required).string() + ": required file is missing");
    }
  }
  Dataset ds;
  ds.name = std::move(name);
  load_movies(raw_dir / "movies.csv", ds);
  if (fs::is_regular_file(raw_dir / "links.csv")) load_links(raw_dir / "links.csv", ds);
  load_ratings(raw_dir / "ratings.csv", ds);
  if (fs::is_regular_file(raw_dir / "tags.csv")) load_tags(raw_dir / "tags.csv", ds);

  const auto genome_tags = raw_dir / "genome-tags.csv";
  const auto genome_scores = raw_dir / "genome-scores.csv";
  const bool has_tags = fs::is_regular_file(genome_tags);
  const bool has_scores = fs::is_regular_file(genome_scores);
  if (has_tags != has_scores) {
    throw DatasetError(raw_dir.string() + ": genome-tags.csv and genome-scores.csv must both be present or both absent");
  }
  if (has_tags) ds.genome = load_genome(genome_tags, genome_scores, ds);
  return ds;
}

}  // namespace recs
