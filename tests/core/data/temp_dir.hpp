// Scratch directories for the data tests: unique per process and per instance, removed on scope exit.
#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace recs::testing {

class TempDir {
 public:
  explicit TempDir(std::string_view tag) {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("recs-test-" + std::string(tag) + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter++));
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
  std::filesystem::path operator/(std::string_view name) const { return path_ / name; }

  void write(std::string_view name, std::string_view content) const {
    std::ofstream out(path_ / name, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

 private:
  std::filesystem::path path_;
};

}  // namespace recs::testing
