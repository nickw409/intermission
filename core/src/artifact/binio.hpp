// Little-endian binary file helpers for artifact files (contracts/artifact.md rule 5).
//
// Every file starts with an 8-byte magic and a u32 format version. Integers are fixed-width
// little-endian; floats are IEEE-754 bit patterns written as little-endian u32/u64. Nothing depends
// on struct layout or host byte order, so files are portable and byte-identical across runs.
// Readers check every length against the bytes actually present and throw ArtifactError on
// truncation, a wrong magic, a wrong version, or trailing bytes.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "recs/artifact.hpp"

namespace recs::binio {

class Writer {
 public:
  Writer(const std::filesystem::path& file, std::string_view magic, std::uint32_t version)
      : path_(file), out_(file, std::ios::binary | std::ios::trunc) {
    if (!out_) throw ArtifactError("cannot open " + file.string() + " for writing");
    if (magic.size() != 8) throw std::logic_error("binio magic must be 8 bytes");
    out_.write(magic.data(), 8);
    u32(version);
  }
  void u32(std::uint32_t v) { put(v, 4); }
  void u64(std::uint64_t v) { put(v, 8); }
  void i64(std::int64_t v) { put(static_cast<std::uint64_t>(v), 8); }
  void f32(float v) { put(std::bit_cast<std::uint32_t>(v), 4); }
  void f64(double v) { put(std::bit_cast<std::uint64_t>(v), 8); }
  void str(std::string_view s) {
    u64(s.size());
    out_.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  /// Flushes and checks the stream; call before the Writer goes out of scope.
  void close() {
    out_.flush();
    if (!out_) throw ArtifactError("write failed: " + path_.string());
    out_.close();
  }

 private:
  void put(std::uint64_t v, int bytes) {
    char b[8];
    for (int i = 0; i < bytes; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    out_.write(b, bytes);
  }
  std::filesystem::path path_;
  std::ofstream out_;
};

class Reader {
 public:
  Reader(const std::filesystem::path& file, std::string_view magic, std::uint32_t version)
      : path_(file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ArtifactError("cannot open " + file.string());
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.size() < 12 || std::string_view(data_.data(), 8) != magic) {
      throw ArtifactError(file.string() + ": bad magic");
    }
    pos_ = 8;
    if (u32() != version) throw ArtifactError(file.string() + ": unsupported version");
  }
  std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
  std::uint64_t u64() { return get(8); }
  std::int64_t i64() { return static_cast<std::int64_t>(get(8)); }
  float f32() { return std::bit_cast<float>(static_cast<std::uint32_t>(get(4))); }
  double f64() { return std::bit_cast<double>(get(8)); }
  std::string str() {
    const std::uint64_t n = u64();
    need(n);
    std::string s(data_.data() + pos_, n);
    pos_ += n;
    return s;
  }
  /// Checks that `count` elements of `elem_size` bytes can still be read (guards huge allocations).
  void need_elems(std::uint64_t count, std::uint64_t elem_size) {
    if (elem_size != 0 && count > (data_.size() - pos_) / elem_size) truncated();
  }
  /// Throws unless every byte was consumed.
  void finish() const {
    if (pos_ != data_.size()) throw ArtifactError(path_.string() + ": trailing bytes");
  }

 private:
  [[noreturn]] void truncated() const { throw ArtifactError(path_.string() + ": truncated"); }
  void need(std::uint64_t n) const {
    if (n > data_.size() - pos_) truncated();
  }
  std::uint64_t get(int bytes) {
    need(static_cast<std::uint64_t>(bytes));
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      v |= static_cast<std::uint64_t>(static_cast<unsigned char>(data_[pos_ + static_cast<std::size_t>(i)])) << (8 * i);
    }
    pos_ += static_cast<std::size_t>(bytes);
    return v;
  }
  std::filesystem::path path_;
  std::vector<char> data_;
  std::size_t pos_ = 0;
};

}  // namespace recs::binio
