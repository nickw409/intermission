// RFC 4180 CSV reader. Workstream A owns the implementation (core/src/data/csv.cpp).
// FROZEN after Wave 0.
//
// Rules: fields separated by ','; records end with LF or CRLF; a field may be enclosed in '"';
// inside a quoted field, '""' is a literal quote and commas/newlines are literal; a UTF-8 BOM at
// the very start of the input is skipped. A final record without a trailing newline is accepted.
// Malformed input (a quote inside an unquoted field, characters after a closing quote, an
// unterminated quoted field, or a wrong field count when one is expected) throws CsvError.
#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace recs {

class CsvError : public std::runtime_error {
 public:
  CsvError(std::string source, std::size_t row, const std::string& what)
      : std::runtime_error(source + ":" + std::to_string(row) + ": " + what),
        source_(std::move(source)), row_(row) {}
  const std::string& source() const { return source_; }
  std::size_t row() const { return row_; }  // 1-based record number; the header is row 1

 private:
  std::string source_;
  std::size_t row_;
};

class CsvReader {
 public:
  /// `expected_fields`: if set, every record must have exactly this many fields.
  CsvReader(std::istream& in, std::string source_name,
            std::optional<std::size_t> expected_fields = std::nullopt);
  /// Reads the next record into `fields`. Returns false at end of input. Throws CsvError.
  bool next(std::vector<std::string>& fields);
  /// 1-based record number of the record last returned by next().
  std::size_t row() const { return row_; }

 private:
  std::istream& in_;
  std::string source_;
  std::optional<std::size_t> expected_;
  std::size_t row_ = 0;
  bool started_ = false;
};

/// Convenience for tests and small inputs: parses a whole string.
std::vector<std::vector<std::string>> parse_csv(std::string_view text, std::string source_name = "<string>",
                                                std::optional<std::size_t> expected_fields = std::nullopt);

}  // namespace recs
