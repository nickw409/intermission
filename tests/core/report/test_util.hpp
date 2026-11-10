// Small helpers shared by workstream C's tests (temporary directories, file I/O).
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <unistd.h>

namespace recs::testing {

/// A fresh directory under the system temp dir, removed (recursively) on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag = "recs") {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            (tag + "-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

}  // namespace recs::testing
