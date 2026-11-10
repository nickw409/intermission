// STUB (Wave 0). Owner: workstream A. Replace with the real implementation.
#include "recs/csv.hpp"
#include "util/todo.hpp"
namespace recs {
CsvReader::CsvReader(std::istream& in, std::string source_name, std::optional<std::size_t> expected_fields)
    : in_(in), source_(std::move(source_name)), expected_(expected_fields) {}
bool CsvReader::next(std::vector<std::string>&) { RECS_TODO("CsvReader::next"); }
std::vector<std::vector<std::string>> parse_csv(std::string_view, std::string, std::optional<std::size_t>) { RECS_TODO("parse_csv"); }
}  // namespace recs
