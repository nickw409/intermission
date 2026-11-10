// SHA-256 (FIPS 180-4). Used for split hashes, cache checksums and artifact manifests.
// FROZEN after Wave 0.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace recs {

class Sha256 {
 public:
  Sha256();
  void update(std::span<const std::uint8_t> data);
  void update(std::string_view s);
  std::array<std::uint8_t, 32> digest();  // finalizes; the object must not be reused afterwards
  std::string hex_digest();                // lowercase hex of digest()

 private:
  void compress(const std::uint8_t* block);
  std::array<std::uint32_t, 8> h_{};
  std::array<std::uint8_t, 64> buf_{};
  std::size_t buf_len_ = 0;
  std::uint64_t total_len_ = 0;
};

std::string sha256_hex(std::string_view data);
std::string sha256_file_hex(const std::filesystem::path& file);  // streams the file; throws on I/O error

}  // namespace recs
