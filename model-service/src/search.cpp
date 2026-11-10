// Title search.
//
// Matching works on a normalized form of the text, so that "amelie", "AMÉLIE" and "Amélie" (in
// either precomposed or decomposed Unicode) all find "Amélie (Fabuleux destin d'Amélie Poulain,
// Le)":
//   1. decode UTF-8 strictly (overlong forms, surrogates and values above U+10FFFF are invalid);
//   2. fold Latin letters to lower-case ASCII without accents, using a table for Latin-1 Supplement
//      and Latin Extended-A (U+00C0..U+017F; "ß" -> "ss", "Æ" -> "ae", "Ł" -> "l");
//   3. drop combining diacritical marks (U+0300..U+036F) and apostrophes;
//   4. turn other ASCII punctuation, controls and common Unicode punctuation into spaces, and
//      collapse runs of spaces.
// Code points outside these ranges (Cyrillic, CJK, ...) are kept unchanged.
//
// MovieLens moves leading articles to the end of a title ("Matrix, The"), so each title is indexed
// twice: as stored, and with the article moved back ("The Matrix"). A query matches a title if it
// is a substring of either form; it is a prefix match if either form starts with it.
//
// Ranking: prefix matches before substring matches, then by popularity rank (most rated first).
// The rank is a permutation of 1..n, so the order is total and deterministic. Cost is one linear
// scan over the catalog per query (about 87k short strings on ml-32m, well under a millisecond),
// then a partial sort of the matches.
#include "search.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

namespace recs::service {
namespace {

// Decodes one code point starting at s[i]. Returns false on invalid UTF-8.
bool decode(std::string_view s, std::size_t& i, char32_t& cp) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    cp = b0;
    ++i;
    return true;
  }
  std::size_t len = 0;
  char32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
    min = 0x10000;
  } else {
    return false;
  }
  if (s.size() - i < len) return false;
  for (std::size_t k = 1; k < len; ++k) {
    const auto b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  i += len;
  return true;
}

void encode(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

struct Fold {
  char32_t lo, hi;
  const char* ascii;  // replacement; " " = separator, "" = drop
};

// Latin-1 Supplement and Latin Extended-A, upper and lower case together.
constexpr std::array<Fold, 52> kFolds = {{
    {0x00C0, 0x00C5, "a"}, {0x00C6, 0x00C6, "ae"}, {0x00C7, 0x00C7, "c"}, {0x00C8, 0x00CB, "e"},
    {0x00CC, 0x00CF, "i"}, {0x00D0, 0x00D0, "d"},  {0x00D1, 0x00D1, "n"}, {0x00D2, 0x00D6, "o"},
    {0x00D7, 0x00D7, " "}, {0x00D8, 0x00D8, "o"},  {0x00D9, 0x00DC, "u"}, {0x00DD, 0x00DD, "y"},
    {0x00DE, 0x00DE, "th"}, {0x00DF, 0x00DF, "ss"}, {0x00E0, 0x00E5, "a"}, {0x00E6, 0x00E6, "ae"},
    {0x00E7, 0x00E7, "c"}, {0x00E8, 0x00EB, "e"},  {0x00EC, 0x00EF, "i"}, {0x00F0, 0x00F0, "d"},
    {0x00F1, 0x00F1, "n"}, {0x00F2, 0x00F6, "o"},  {0x00F7, 0x00F7, " "}, {0x00F8, 0x00F8, "o"},
    {0x00F9, 0x00FC, "u"}, {0x00FD, 0x00FD, "y"},  {0x00FE, 0x00FE, "th"}, {0x00FF, 0x00FF, "y"},
    {0x0100, 0x0105, "a"}, {0x0106, 0x010D, "c"},  {0x010E, 0x0111, "d"}, {0x0112, 0x011B, "e"},
    {0x011C, 0x0123, "g"}, {0x0124, 0x0127, "h"},  {0x0128, 0x0131, "i"}, {0x0132, 0x0133, "ij"},
    {0x0134, 0x0135, "j"}, {0x0136, 0x0138, "k"},  {0x0139, 0x0142, "l"}, {0x0143, 0x014B, "n"},
    {0x014C, 0x0151, "o"}, {0x0152, 0x0153, "oe"}, {0x0154, 0x0159, "r"}, {0x015A, 0x0161, "s"},
    {0x0162, 0x0167, "t"}, {0x0168, 0x0173, "u"},  {0x0174, 0x0175, "w"}, {0x0176, 0x0178, "y"},
    {0x0179, 0x017E, "z"}, {0x017F, 0x017F, "s"},
    // Punctuation and symbols in Latin-1 (no-break space, quotes, ...) separate words.
    {0x00A0, 0x00BF, " "},
    // Combining diacritical marks (decomposed accents) are dropped.
    {0x0300, 0x036F, ""},
}};

bool is_apostrophe(char32_t cp) { return cp == '\'' || cp == '`' || cp == 0x2018 || cp == 0x2019 || cp == 0x00B4; }

// Appends the folded form of one code point. `pending_space` collapses separators.
void fold_into(char32_t cp, std::string& out, bool& pending_space) {
  const auto emit = [&](std::string_view s) {
    if (pending_space && !out.empty()) out += ' ';
    pending_space = false;
    out += s;
  };
  if (is_apostrophe(cp)) return;
  if (cp < 0x80) {
    const char c = static_cast<char>(cp);
    if (c >= 'A' && c <= 'Z') {
      emit(std::string(1, static_cast<char>(c - 'A' + 'a')));
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      emit(std::string(1, c));
    } else {
      pending_space = true;
    }
    return;
  }
  for (const Fold& f : kFolds) {
    if (cp >= f.lo && cp <= f.hi) {
      const std::string_view r = f.ascii;
      if (r == " ") {
        pending_space = true;
      } else if (!r.empty()) {
        emit(r);
      }
      return;
    }
  }
  if (cp >= 0x2000 && cp <= 0x206F) {  // General Punctuation: dashes, spaces, quotes
    pending_space = true;
    return;
  }
  std::string enc;
  encode(cp, enc);
  emit(enc);
}

// lenient = true replaces invalid bytes by a separator instead of failing (for catalog titles).
std::optional<std::string> normalize(std::string_view s, bool lenient) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  std::size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    if (!decode(s, i, cp)) {
      if (!lenient) return std::nullopt;
      ++i;
      pending_space = true;
      continue;
    }
    fold_into(cp, out, pending_space);
  }
  return out;
}

constexpr std::array<std::string_view, 20> kArticles = {"The", "A",   "An",  "Les", "La",  "Le",  "L'",
                                                        "Il",  "El",  "Die", "Der", "Das", "Los", "Las",
                                                        "Un",  "Une", "Det", "Den", "Het", "Gli"};

}  // namespace

std::optional<std::string> normalize_text(std::string_view s) { return normalize(s, false); }

std::optional<std::size_t> utf8_length(std::string_view s) {
  std::size_t n = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    if (!decode(s, i, cp)) return std::nullopt;
    ++n;
  }
  return n;
}

std::optional<std::string> article_first_title(std::string_view title) {
  const std::size_t paren = title.find(" (");
  const std::string_view main = title.substr(0, paren);
  const std::string_view rest = paren == std::string_view::npos ? std::string_view{} : title.substr(paren);
  for (std::string_view art : kArticles) {
    if (main.size() < art.size() + 3) continue;
    const std::string_view tail = main.substr(main.size() - art.size() - 2);
    if (tail.substr(0, 2) != ", " || tail.substr(2) != art) continue;
    std::string out(art);
    if (art.back() != '\'') out += ' ';
    out += main.substr(0, main.size() - art.size() - 2);
    out += rest;
    return out;
  }
  return std::nullopt;
}

SearchIndex::SearchIndex(const Catalog& catalog) {
  entries_.resize(catalog.movies.size());
  for (std::size_t i = 0; i < catalog.movies.size(); ++i) {
    Entry& e = entries_[i];
    e.title = *normalize(catalog.movies[i].title, true);
    if (const auto alt = article_first_title(catalog.movies[i].title)) e.alt_title = *normalize(*alt, true);
    e.rank = catalog.popularity_rank[i];
  }
}

std::vector<ItemIdx> SearchIndex::search(std::string_view query, std::size_t limit) const {
  const auto q = normalize(query, true);
  if (!q || q->empty() || limit == 0) return {};
  // (0 = prefix / 1 = substring, popularity rank, item)
  std::vector<std::tuple<int, std::uint32_t, ItemIdx>> hits;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const Entry& e = entries_[i];
    const bool prefix = e.title.starts_with(*q) || (!e.alt_title.empty() && e.alt_title.starts_with(*q));
    if (prefix) {
      hits.emplace_back(0, e.rank, static_cast<ItemIdx>(i));
    } else if (e.title.find(*q) != std::string::npos || e.alt_title.find(*q) != std::string::npos) {
      hits.emplace_back(1, e.rank, static_cast<ItemIdx>(i));
    }
  }
  const std::size_t take = std::min(limit, hits.size());
  std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(take), hits.end());
  std::vector<ItemIdx> out;
  out.reserve(take);
  for (std::size_t i = 0; i < take; ++i) out.push_back(std::get<2>(hits[i]));
  return out;
}

}  // namespace recs::service
