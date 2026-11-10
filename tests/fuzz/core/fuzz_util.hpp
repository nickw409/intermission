// Shared helpers for the librecs loader fuzzers (tests/fuzz/core/*.cpp).
//
// The loaders read files, so each harness writes the fuzz input into a per-process scratch
// directory and calls the real loader on it. Loaders reject bad input by throwing; every
// std::exception is an accepted outcome. What libFuzzer hunts for is everything else: ASan/UBSan
// reports, aborts, hangs and huge allocations (-malloc_limit_mb).
//
// Several formats end in a SHA-256 of the preceding bytes, which random mutation would almost
// never get right. The harnesses for those formats treat the fuzz input as the body and append
// (seal) the correct digest, so the parser behind the checksum is what gets exercised.
#pragma once

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "recs/sha256.hpp"

namespace recs::fuzz {

namespace fs = std::filesystem;

inline std::string_view as_view(const std::uint8_t* data, std::size_t size) {
  return {reinterpret_cast<const char*>(data), size};
}

/// One scratch directory per process (libFuzzer -fork/-jobs workers each get their own).
inline const fs::path& scratch_dir() {
  static const fs::path dir = [] {
    fs::path d = fs::temp_directory_path() / ("recs-fuzz-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
  }();
  return dir;
}

/// A fresh, empty subdirectory of the scratch directory.
inline fs::path fresh_dir(std::string_view name) {
  const fs::path d = scratch_dir() / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

inline void write_file(const fs::path& p, std::string_view bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// body + SHA-256(body).
inline std::string sealed(std::string_view body) {
  Sha256 sha;
  sha.update(body);
  const auto d = sha.digest();
  std::string out(body);
  out.append(reinterpret_cast<const char*>(d.data()), d.size());
  return out;
}

/// Input layout shared by the model harnesses whose model saves a config.json next to its binary
/// file: u16 little-endian length L, then L bytes of config.json, then the binary file.
struct SplitInput {
  std::string_view config;
  std::string_view binary;
};

inline SplitInput split_config(std::string_view in) {
  if (in.size() < 2) return {{}, in};
  const std::size_t len = static_cast<std::uint8_t>(in[0]) | (static_cast<std::size_t>(static_cast<std::uint8_t>(in[1])) << 8);
  in.remove_prefix(2);
  const std::size_t n = len < in.size() ? len : in.size();
  return {in.substr(0, n), in.substr(n)};
}

inline std::string join_config(std::string_view config, std::string_view binary) {
  std::string out;
  out.push_back(static_cast<char>(config.size() & 0xff));
  out.push_back(static_cast<char>((config.size() >> 8) & 0xff));
  out.append(config);
  out.append(binary);
  return out;
}

}  // namespace recs::fuzz
