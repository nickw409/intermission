// RFC 4180 CSV reader.
//
// The reader is a small state machine driven one byte at a time straight from the stream's
// buffer (std::streambuf::sbumpc), which is fast enough for the 877 MB ml-32m ratings file and
// keeps no state between records except the record counter. For each field we are in one of four
// states:
//
//   FieldStart   nothing read yet for this field
//   Unquoted     inside a field that did not start with '"'
//   Quoted       inside a field that started with '"'
//   QuoteInQuoted  just saw '"' inside a quoted field: either the first half of an escaped '""'
//                  or the closing quote
//
// Transitions (anything not listed is appended to the current field):
//
//   FieldStart:    '"' -> Quoted;  ',' -> end field;  LF/CRLF/EOF -> end record
//   Unquoted:      ',' -> end field;  LF/CRLF/EOF -> end record;  '"' -> error (stray quote)
//   Quoted:        '"' -> QuoteInQuoted;  EOF -> error (unterminated);  CR and LF are literal
//   QuoteInQuoted: '"' -> append '"', Quoted;  ',' -> end field;  LF/CRLF/EOF -> end record;
//                  anything else -> error (characters after the closing quote)
//
// A CR that is not followed by LF is an ordinary character inside a field (it is never a record
// terminator on its own), except right at the end of the input, where it ends the last record.
// Row numbers count records, not lines: a quoted field with embedded newlines is still one row,
// and errors report the row on which the offending record started.
//
// Reference: RFC 4180, "Common Format and MIME Type for Comma-Separated Values (CSV) Files".
#include "recs/csv.hpp"

#include <sstream>
#include <string>

namespace recs {

namespace {

using Traits = std::char_traits<char>;
constexpr Traits::int_type kEof = Traits::eof();

enum class State { FieldStart, Unquoted, Quoted, QuoteInQuoted };

// Consumes an LF after a CR if there is one. Returns true if the CR ends the record (followed by
// LF or by the end of the input).
bool cr_ends_record(std::streambuf* sb) {
  const auto next = sb->sgetc();
  if (next == '\n') {
    sb->sbumpc();
    return true;
  }
  return next == kEof;
}

}  // namespace

CsvReader::CsvReader(std::istream& in, std::string source_name, std::optional<std::size_t> expected_fields)
    : in_(in), source_(std::move(source_name)), expected_(expected_fields) {}

bool CsvReader::next(std::vector<std::string>& fields) {
  std::streambuf* sb = in_.rdbuf();
  if (sb == nullptr) return false;

  // Bytes of a partial (non-)BOM consumed while checking for a UTF-8 BOM at the very start.
  std::string bom_prefix;
  if (!started_) {
    started_ = true;
    if (sb->sgetc() == 0xEF) {
      bom_prefix.push_back(static_cast<char>(sb->sbumpc()));
      if (sb->sgetc() == 0xBB) {
        bom_prefix.push_back(static_cast<char>(sb->sbumpc()));
        if (sb->sgetc() == 0xBF) {
          sb->sbumpc();
          bom_prefix.clear();  // a real BOM: dropped
        }
      }
    }
  }
  if (bom_prefix.empty() && sb->sgetc() == kEof) return false;

  ++row_;
  std::size_t n = 0;  // fields completed so far in this record
  // Starts a new field, reusing the caller's strings so their capacity is kept across records.
  auto begin_field = [&]() -> std::string& {
    if (fields.size() <= n) fields.emplace_back();
    std::string& f = fields[n];
    f.clear();
    return f;
  };
  auto fail = [&](const std::string& what) { throw CsvError(source_, row_, what); };

  std::string* field = &begin_field();
  State state = State::FieldStart;
  if (!bom_prefix.empty()) {
    field->append(bom_prefix);  // not special characters, so the field is unquoted
    state = State::Unquoted;
  }

  bool record_done = false;
  while (!record_done) {
    const auto c = sb->sbumpc();
    switch (state) {
      case State::FieldStart:
      case State::Unquoted:
        if (c == ',') {
          ++n;
          field = &begin_field();
          state = State::FieldStart;
        } else if (c == '\n' || c == kEof) {
          record_done = true;
        } else if (c == '\r' && cr_ends_record(sb)) {
          record_done = true;
        } else if (c == '"') {
          if (state == State::Unquoted) fail("quote inside an unquoted field " + std::to_string(n + 1));
          state = State::Quoted;
        } else {
          field->push_back(Traits::to_char_type(c));
          state = State::Unquoted;
        }
        break;
      case State::Quoted:
        if (c == '"') {
          state = State::QuoteInQuoted;
        } else if (c == kEof) {
          fail("unterminated quoted field " + std::to_string(n + 1));
        } else {
          field->push_back(Traits::to_char_type(c));
        }
        break;
      case State::QuoteInQuoted:
        if (c == '"') {
          field->push_back('"');
          state = State::Quoted;
        } else if (c == ',') {
          ++n;
          field = &begin_field();
          state = State::FieldStart;
        } else if (c == '\n' || c == kEof) {
          record_done = true;
        } else if (c == '\r' && cr_ends_record(sb)) {
          record_done = true;
        } else {
          fail("characters after the closing quote of field " + std::to_string(n + 1));
        }
        break;
    }
  }
  ++n;
  fields.resize(n);
  if (expected_ && n != *expected_) {
    fail(std::to_string(n) + " fields, expected " + std::to_string(*expected_));
  }
  return true;
}

std::vector<std::vector<std::string>> parse_csv(std::string_view text, std::string source_name,
                                                std::optional<std::size_t> expected_fields) {
  std::istringstream in{std::string(text)};
  CsvReader reader(in, std::move(source_name), expected_fields);
  std::vector<std::vector<std::string>> out;
  std::vector<std::string> fields;
  while (reader.next(fields)) out.push_back(fields);
  return out;
}

}  // namespace recs
