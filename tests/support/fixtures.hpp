// Paths to test fixtures (set by tests/CMakeLists.txt).
#pragma once
#include <filesystem>

namespace recs::testing {
inline std::filesystem::path fixtures_dir() { return RECS_FIXTURES_DIR; }
inline std::filesystem::path tiny_dir() { return fixtures_dir() / "tiny"; }
inline std::filesystem::path csv_fixtures_dir() { return fixtures_dir() / "csv"; }
inline std::filesystem::path contracts_dir() { return RECS_CONTRACTS_DIR; }
inline std::filesystem::path data_dir() { return RECS_DATA_DIR; }
}  // namespace recs::testing
