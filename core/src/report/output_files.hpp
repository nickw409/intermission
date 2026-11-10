// Output-file helpers shared by the report writers (core/src/report/) and the recs-train
// commands that write report-side files (train/cmd_eval.cpp, train/cmd_compare.cpp).
// Header-only so the CLI can use it without a public librecs header.
//
// write_file_atomic: the file appears under its final name complete or not at all.
//   1. Create a uniquely named temp file next to the target with O_CREAT | O_EXCL.
//   2. Write everything, check for errors, fsync the file.
//   3. Publish it: with NoReplace, link(temp, final), which fails with EEXIST if the final name
//      already exists (so two runs can never overwrite each other, unlike exists()-then-rename);
//      otherwise rename(temp, final), which atomically replaces any old file.
//   4. Remove the temp name, fsync the directory.
// On any error the temp file is removed and std::runtime_error is thrown.
//
// paths_nested: whether one directory is the other or lies inside it, comparing canonical paths
// (symlinks resolved, `..` removed), so "data/reports/../reports/x" and a symlinked alias are
// caught, while an unrelated directory that merely has a component named "reports" is not.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace recs::output {

enum class Mode { Replace, NoReplace };

namespace detail {

inline void fsync_dir(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return;  // best effort: the file itself is already durable
  ::fsync(fd);
  ::close(fd);
}

[[noreturn]] inline void fail(const std::string& what, int err) {
  throw std::runtime_error(what + ": " + std::strerror(err));
}

}  // namespace detail

inline std::filesystem::path write_file_atomic(const std::filesystem::path& final_path, std::string_view text,
                                               Mode mode) {
  namespace fs = std::filesystem;
  static std::atomic<unsigned> counter{0};
  const fs::path dir = final_path.has_parent_path() ? final_path.parent_path() : fs::path(".");
  const fs::path tmp = dir / ("." + final_path.filename().string() + ".tmp-" + std::to_string(::getpid()) + "-" +
                              std::to_string(counter++));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) detail::fail("cannot create " + tmp.string(), errno);
  auto abort_with = [&](const std::string& what, int err) {
    ::close(fd);
    ::unlink(tmp.c_str());
    detail::fail(what, err);
  };
  for (std::size_t off = 0; off < text.size();) {
    const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      abort_with("cannot write " + tmp.string(), errno);
    }
    off += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) abort_with("cannot fsync " + tmp.string(), errno);
  if (::close(fd) != 0) {
    const int err = errno;
    ::unlink(tmp.c_str());
    detail::fail("cannot close " + tmp.string(), err);
  }
  if (mode == Mode::NoReplace) {
    if (::link(tmp.c_str(), final_path.c_str()) != 0) {
      const int err = errno;
      ::unlink(tmp.c_str());
      if (err == EEXIST) throw std::runtime_error("refusing to overwrite " + final_path.string());
      detail::fail("cannot publish " + final_path.string(), err);
    }
    ::unlink(tmp.c_str());
  } else if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
    const int err = errno;
    ::unlink(tmp.c_str());
    detail::fail("cannot publish " + final_path.string(), err);
  }
  detail::fsync_dir(dir);
  return final_path;
}

/// True if a == b or one lies inside the other (canonical, component-wise comparison).
inline bool paths_nested(const std::filesystem::path& a, const std::filesystem::path& b) {
  namespace fs = std::filesystem;
  const fs::path ca = fs::weakly_canonical(fs::absolute(a));
  const fs::path cb = fs::weakly_canonical(fs::absolute(b));
  auto is_prefix = [](const fs::path& p, const fs::path& q) {  // p is q or an ancestor of q
    auto pi = p.begin(), qi = q.begin();
    for (; pi != p.end(); ++pi, ++qi) {
      if (pi->empty()) continue;  // trailing separator
      if (qi == q.end() || *pi != *qi) return false;
    }
    return true;
  };
  return is_prefix(ca, cb) || is_prefix(cb, ca);
}

}  // namespace recs::output
