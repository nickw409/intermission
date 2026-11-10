// Binary dataset cache: lets later runs skip CSV parsing (ml-32m parses in about 10 s in release,
// the cache loads in about 5 s).
//
// Layout (format version 2). All integers are little-endian; a "str" is a u64 byte length followed
// by the bytes; "opt<T>" is a u8 flag (0 or 1) followed by a T that is 0 when the flag is 0.
//
//   header   char[8] magic "RECSDS01"                                           offset 0
//            u32 version, u32 flags (bit 0: genome present)                      offsets 8, 12
//            u32 n_users, u32 n_items, u64 nnz, u64 n_tag_applications           offsets 16, 20, 24, 32
//   payload  str name                                                            offset 40
//            u64 n_sources, then per raw file: str file name, u8 present, u64 size, i64 mtime (ns)
//            i64[n_users] user external ids, i64[n_items] item external ids (dense order)
//            by_user CSR: u64[n_users + 1] indptr, u32[nnz] indices, f32[nnz] values, i64[nnz] ts
//            n_items x movie: i64 movie_id, str title, opt<i32> year, u32 n_genres, str[n_genres],
//                             opt<i64> imdb_id, opt<i64> tmdb_id
//            n_tag_applications x tag: i64 user_id, u32 item, str tag, i64 ts
//            if genome: u32 n_items (== header n_items), u32 n_tags, str[n_tags] tag names,
//                       f32[n_items * n_tags] relevance
//   trailer  SHA-256 of header + payload (32 bytes)
//
// Version history: 1 had no source fingerprints. 2 added them (after the name).
//
// by_item is not stored: it is always transpose(by_user), so it is rebuilt on load. This halves
// the file (ml-32m: ~0.5 GB instead of ~1 GB) and makes the CSR/CSC invariant hold by construction.
//
// Source fingerprints: load_or_build records the size and modification time of every raw file
// load_movielens may read (present or not). A cache whose fingerprints differ from the raw
// directory's current state is stale and gets rebuilt. A cache written by plain save_cache has no
// fingerprints, so load_cache accepts it but load_or_build never trusts it.
//
// save_cache checks the dataset against the same rules load_cache enforces before it opens any
// file, writes to a uniquely named temporary file, fsyncs it and renames it over the target, so a
// reader sees either the old cache or the complete new one. On any failure the temporary file is
// removed.
//
// load_cache reads the file once, hashing as it goes. Every length read from the file is checked
// against the bytes that remain before anything is allocated, so a corrupt length can never cause
// a huge allocation. The checksum, trailing data and the dataset invariants (unique ids, a valid
// CSR, ratings in {0.5, ..., 5.0}, in-range item indices, genome dimensions matching the item
// count, relevance in [0, 1]) are all verified before the Dataset is returned.
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "recs/dataset.hpp"
#include "recs/sha256.hpp"
#include "recs/sparse.hpp"

namespace recs {

namespace {

static_assert(std::endian::native == std::endian::little, "the cache format assumes a little-endian host");

constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'D', 'S', '0', '1'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kFlagGenome = 1u;
constexpr std::size_t kDigestSize = 32;
constexpr std::size_t kBufferSize = 1 << 20;

class Writer {
 public:
  explicit Writer(const fs::path& file) : file_(file), buffer_(kBufferSize) {
    out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    out_.open(file, std::ios::binary | std::ios::trunc);
    if (!out_) throw DatasetError(file.string() + ": cannot open cache file for writing");
  }

  void bytes(const void* data, std::size_t n) {
    if (n == 0) return;
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    sha_.update(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), n));
  }
  template <class T>
  void pod(T v) {
    static_assert(std::is_arithmetic_v<T>);
    bytes(&v, sizeof v);
  }
  template <class T>
  void array(const std::vector<T>& v) {
    bytes(v.data(), v.size() * sizeof(T));
  }
  void str(const std::string& s) {
    pod<std::uint64_t>(s.size());
    bytes(s.data(), s.size());
  }
  template <class T>
  void opt(const std::optional<T>& v) {
    pod<std::uint8_t>(v ? 1 : 0);
    pod<T>(v.value_or(T{}));
  }

  /// Appends the checksum trailer and closes the file.
  void finish() {
    const auto digest = sha_.digest();
    out_.write(reinterpret_cast<const char*>(digest.data()), static_cast<std::streamsize>(digest.size()));
    out_.close();
    if (!out_) throw DatasetError(file_.string() + ": error writing cache file");
  }

 private:
  fs::path file_;
  std::vector<char> buffer_;
  std::ofstream out_;
  Sha256 sha_;
};

class Reader {
 public:
  explicit Reader(const fs::path& file) : file_(file), buffer_(kBufferSize) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) throw DatasetError(file.string() + ": cannot read cache file: " + ec.message());
    in_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    in_.open(file, std::ios::binary);
    if (!in_) throw DatasetError(file.string() + ": cannot open cache file");
    if (size < kDigestSize) fail("file is truncated");
    remaining_ = size - kDigestSize;
  }

  [[noreturn]] void fail(const std::string& what) const {
    throw DatasetError(file_.string() + ": invalid dataset cache: " + what);
  }

  void bytes(void* data, std::size_t n) {
    if (n > remaining_) fail("file is truncated");
    if (n == 0) return;
    in_.read(static_cast<char*>(data), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in_.gcount()) != n) fail("file is truncated");
    sha_.update(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), n));
    remaining_ -= n;
  }
  template <class T>
  T pod() {
    static_assert(std::is_arithmetic_v<T>);
    T v{};
    bytes(&v, sizeof v);
    return v;
  }
  template <class T>
  void array(std::vector<T>& v, std::uint64_t count) {
    if (count > remaining_ / sizeof(T)) fail("array length exceeds file size");
    v.resize(static_cast<std::size_t>(count));
    bytes(v.data(), v.size() * sizeof(T));
  }
  std::string str() {
    const auto n = pod<std::uint64_t>();
    if (n > remaining_) fail("string length exceeds file size");
    std::string s(static_cast<std::size_t>(n), '\0');
    bytes(s.data(), s.size());
    return s;
  }
  template <class T>
  std::optional<T> opt() {
    const auto flag = pod<std::uint8_t>();
    const auto v = pod<T>();
    if (flag > 1) fail("bad optional flag");
    if (flag == 0) {
      if (v != T{}) fail("absent optional has a value");
      return std::nullopt;
    }
    return v;
  }
  /// Fails unless at most `min_bytes_each * count` bytes would still fit; guards reserve().
  void check_count(std::uint64_t count, std::uint64_t min_bytes_each) const {
    if (count > remaining_ / min_bytes_each) fail("record count exceeds file size");
  }

  /// Verifies there is no unread payload and that the trailer matches the checksum.
  void finish() {
    if (remaining_ != 0) fail("unexpected data after the payload");
    std::array<std::uint8_t, kDigestSize> stored{};
    in_.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    if (static_cast<std::size_t>(in_.gcount()) != stored.size()) fail("file is truncated");
    if (in_.peek() != std::char_traits<char>::eof()) fail("unexpected data after the checksum");
    if (sha_.digest() != stored) fail("checksum mismatch");
  }

 private:
  fs::path file_;
  std::vector<char> buffer_;
  std::ifstream in_;
  Sha256 sha_;
  std::uint64_t remaining_ = 0;  // payload bytes not yet read (excludes the trailer)
};

/// Size and modification time of one raw input file, used to detect a stale cache.
struct SourceFile {
  std::string name;
  bool present = false;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  bool operator==(const SourceFile&) const = default;
};
using Sources = std::vector<SourceFile>;

/// Every file load_movielens may read, in a fixed order.
Sources fingerprint(const fs::path& raw_dir) {
  Sources out;
  for (const char* name : {"movies.csv", "ratings.csv", "links.csv", "tags.csv", "genome-tags.csv",
                           "genome-scores.csv"}) {
    SourceFile s;
    s.name = name;
    const auto path = raw_dir / name;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      s.present = true;
      s.size = fs::file_size(path, ec);
      if (ec) s.size = 0;
      const auto t = fs::last_write_time(path, ec);
      if (!ec) s.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }
    out.push_back(std::move(s));
  }
  return out;
}

bool valid_rating(float v) {
  // Also false for NaN. Ratings are multiples of 0.5, so 2v is an exact integer.
  return v >= 0.5f && v <= 5.0f && 2.0f * v == std::floor(2.0f * v);
}

bool valid_relevance(float v) { return v >= 0.0f && v <= 1.0f; }  // false for NaN

/// The rules load_cache enforces, checked up front by save_cache. Throws DatasetError.
void check_saveable(const Dataset& ds) {
  const auto fail = [](const std::string& what) { throw DatasetError("save_cache: " + what); };
  if (ds.users.to_dense.size() != ds.users.to_external.size()) fail("user id map is inconsistent");
  if (ds.items.to_dense.size() != ds.items.to_external.size()) fail("item id map is inconsistent");
  if (ds.movies.size() != ds.n_items()) fail("movies.size() != n_items");
  for (std::uint32_t i = 0; i < ds.n_items(); ++i) {
    if (ds.movies[i].movie_id != ds.items.to_external[i]) fail("movie id does not match the item id map");
  }
  const Csr& m = ds.by_user;
  if (m.n_rows != ds.n_users() || m.n_cols != ds.n_items()) fail("rating matrix shape != n_users x n_items");
  try {
    validate(m);
  } catch (const SparseError& e) {
    fail(std::string("rating matrix: ") + e.what());
  }
  for (const float v : m.values) {
    if (!valid_rating(v)) fail("rating value " + std::to_string(v) + " out of range");
  }
  for (const auto& t : ds.tags) {
    if (t.item >= ds.n_items()) fail("tag item index out of range");
  }
  if (ds.genome) {
    const auto& g = *ds.genome;
    if (g.n_items != ds.n_items()) fail("genome n_items != n_items");
    if (g.tag_names.size() != g.n_tags) fail("genome tag_names.size() != n_tags");
    if (g.relevance.size() != static_cast<std::uint64_t>(g.n_items) * g.n_tags) fail("genome relevance size");
    for (const float v : g.relevance) {
      if (!valid_relevance(v)) fail("genome relevance " + std::to_string(v) + " out of [0, 1]");
    }
  }
}

void read_id_map(Reader& r, IdMap& map, std::uint32_t n, const char* what) {
  std::vector<std::int64_t> ids;
  r.array(ids, n);
  map.to_dense.reserve(n);
  for (const auto id : ids) {
    const auto before = map.size();
    if (map.insert(id) != before) r.fail(std::string("duplicate ") + what + " id " + std::to_string(id));
  }
}

/// Flushes a closed file to stable storage. fsync works on a read-only descriptor on Linux.
void fsync_path(const fs::path& p, bool directory) {
  const int fd = ::open(p.c_str(), directory ? (O_RDONLY | O_DIRECTORY) : O_RDONLY);
  if (fd < 0) {
    if (directory) return;  // best effort for the directory entry
    throw DatasetError(p.string() + ": cannot reopen cache file to sync it");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0 && !directory) throw DatasetError(p.string() + ": fsync failed");
}

fs::path unique_temp_path(const fs::path& file) {
  std::random_device rd;
  const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  char suffix[17];
  std::snprintf(suffix, sizeof suffix, "%016llx", static_cast<unsigned long long>(r));
  fs::path tmp = file;
  tmp += ".tmp-" + std::to_string(::getpid()) + "-" + suffix;
  return tmp;
}

void write_payload(Writer& w, const Dataset& ds, const Sources& sources) {
  w.bytes(kMagic, sizeof kMagic);
  w.pod<std::uint32_t>(kVersion);
  w.pod<std::uint32_t>(ds.genome ? kFlagGenome : 0u);
  w.pod<std::uint32_t>(ds.n_users());
  w.pod<std::uint32_t>(ds.n_items());
  w.pod<std::uint64_t>(ds.n_ratings());
  w.pod<std::uint64_t>(ds.tags.size());

  w.str(ds.name);
  w.pod<std::uint64_t>(sources.size());
  for (const auto& s : sources) {
    w.str(s.name);
    w.pod<std::uint8_t>(s.present ? 1 : 0);
    w.pod<std::uint64_t>(s.size);
    w.pod<std::int64_t>(s.mtime_ns);
  }
  w.array(ds.users.to_external);
  w.array(ds.items.to_external);
  w.array(ds.by_user.indptr);
  w.array(ds.by_user.indices);
  w.array(ds.by_user.values);
  w.array(ds.by_user.ts);
  for (const auto& m : ds.movies) {
    w.pod<std::int64_t>(m.movie_id);
    w.str(m.title);
    w.opt<std::int32_t>(m.year);
    w.pod<std::uint32_t>(static_cast<std::uint32_t>(m.genres.size()));
    for (const auto& g : m.genres) w.str(g);
    w.opt<std::int64_t>(m.imdb_id);
    w.opt<std::int64_t>(m.tmdb_id);
  }
  for (const auto& t : ds.tags) {
    w.pod<std::int64_t>(t.user_id);
    w.pod<std::uint32_t>(t.item);
    w.str(t.tag);
    w.pod<std::int64_t>(t.ts);
  }
  if (ds.genome) {
    const auto& g = *ds.genome;
    w.pod<std::uint32_t>(g.n_items);
    w.pod<std::uint32_t>(g.n_tags);
    for (const auto& name : g.tag_names) w.str(name);
    w.array(g.relevance);
  }
  w.finish();
}

void write_cache(const Dataset& ds, const fs::path& file, const Sources& sources) {
  check_saveable(ds);  // before any file is touched
  if (file.has_parent_path()) fs::create_directories(file.parent_path());
  const fs::path tmp = unique_temp_path(file);
  try {
    {
      Writer w(tmp);
      write_payload(w, ds, sources);
    }
    fsync_path(tmp, false);
    fs::rename(tmp, file);
  } catch (...) {
    std::error_code ec;
    fs::remove(tmp, ec);
    throw;
  }
  fsync_path(file.has_parent_path() ? file.parent_path() : fs::path("."), true);
}

Dataset read_cache(const fs::path& file, Sources* sources_out) {
  Reader r(file);

  char magic[sizeof kMagic];
  r.bytes(magic, sizeof magic);
  if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) r.fail("bad magic (not a dataset cache)");
  const auto version = r.pod<std::uint32_t>();
  if (version != kVersion) {
    r.fail("unsupported format version " + std::to_string(version) + " (expected " + std::to_string(kVersion) + ")");
  }
  const auto flags = r.pod<std::uint32_t>();
  if ((flags & ~kFlagGenome) != 0) r.fail("unknown flags");
  const auto n_users = r.pod<std::uint32_t>();
  const auto n_items = r.pod<std::uint32_t>();
  const auto nnz = r.pod<std::uint64_t>();
  const auto n_tags = r.pod<std::uint64_t>();

  Dataset ds;
  ds.name = r.str();

  Sources sources;
  const auto n_sources = r.pod<std::uint64_t>();
  r.check_count(n_sources, 8 + 1 + 8 + 8);
  sources.reserve(static_cast<std::size_t>(n_sources));
  for (std::uint64_t k = 0; k < n_sources; ++k) {
    SourceFile s;
    s.name = r.str();
    const auto present = r.pod<std::uint8_t>();
    if (present > 1) r.fail("bad source flag");
    s.present = present == 1;
    s.size = r.pod<std::uint64_t>();
    s.mtime_ns = r.pod<std::int64_t>();
    sources.push_back(std::move(s));
  }

  read_id_map(r, ds.users, n_users, "user");
  read_id_map(r, ds.items, n_items, "item");

  Csr& m = ds.by_user;
  m.n_rows = n_users;
  m.n_cols = n_items;
  r.array(m.indptr, static_cast<std::uint64_t>(n_users) + 1);
  r.array(m.indices, nnz);
  r.array(m.values, nnz);
  r.array(m.ts, nnz);
  try {
    validate(m);
  } catch (const SparseError& e) {
    r.fail(std::string("rating matrix: ") + e.what());
  }
  for (const float v : m.values) {
    if (!valid_rating(v)) r.fail("rating value " + std::to_string(v) + " out of range");
  }

  r.check_count(n_items, 8 + 8 + 5 + 4 + 9 + 9);  // smallest possible movie record
  ds.movies.reserve(n_items);
  for (std::uint32_t i = 0; i < n_items; ++i) {
    MovieMeta mm;
    mm.movie_id = r.pod<std::int64_t>();
    if (mm.movie_id != ds.items.to_external[i]) r.fail("movie id does not match the item id map");
    mm.title = r.str();
    if (const auto y = r.opt<std::int32_t>()) mm.year = static_cast<int>(*y);
    const auto n_genres = r.pod<std::uint32_t>();
    r.check_count(n_genres, 8);
    mm.genres.reserve(n_genres);
    for (std::uint32_t g = 0; g < n_genres; ++g) mm.genres.push_back(r.str());
    mm.imdb_id = r.opt<std::int64_t>();
    mm.tmdb_id = r.opt<std::int64_t>();
    ds.movies.push_back(std::move(mm));
  }

  r.check_count(n_tags, 8 + 4 + 8 + 8);  // smallest possible tag record
  ds.tags.reserve(static_cast<std::size_t>(n_tags));
  for (std::uint64_t k = 0; k < n_tags; ++k) {
    TagApplication t;
    t.user_id = r.pod<std::int64_t>();
    t.item = r.pod<std::uint32_t>();
    if (t.item >= n_items) r.fail("tag item index out of range");
    t.tag = r.str();
    t.ts = r.pod<std::int64_t>();
    ds.tags.push_back(std::move(t));
  }

  if ((flags & kFlagGenome) != 0) {
    GenomeMatrix g;
    g.n_items = r.pod<std::uint32_t>();
    if (g.n_items != n_items) {
      r.fail("genome n_items " + std::to_string(g.n_items) + " != n_items " + std::to_string(n_items));
    }
    g.n_tags = r.pod<std::uint32_t>();
    r.check_count(g.n_tags, 8);
    g.tag_names.reserve(g.n_tags);
    for (std::uint32_t k = 0; k < g.n_tags; ++k) g.tag_names.push_back(r.str());
    r.array(g.relevance, static_cast<std::uint64_t>(g.n_items) * g.n_tags);
    for (const float v : g.relevance) {
      if (!valid_relevance(v)) r.fail("genome relevance " + std::to_string(v) + " out of [0, 1]");
    }
    ds.genome = std::move(g);
  }

  r.finish();
  ds.by_item = transpose(ds.by_user);
  if (sources_out != nullptr) *sources_out = std::move(sources);
  return ds;
}

}  // namespace

void save_cache(const Dataset& ds, const fs::path& file) { write_cache(ds, file, {}); }

Dataset load_cache(const fs::path& file) { return read_cache(file, nullptr); }

Dataset load_or_build(const fs::path& raw_dir, const fs::path& cache_file, std::string name) {
  // Fingerprint before parsing: if a file changes during the parse, the next run sees the change.
  const Sources current = fingerprint(raw_dir);
  if (fs::exists(cache_file)) {
    try {
      Sources stored;
      Dataset ds = read_cache(cache_file, &stored);
      if (ds.name != name) {
        std::cerr << "recs: cache " << cache_file.string() << " holds dataset '" << ds.name << "', not '" << name
                  << "'; rebuilding\n";
      } else if (stored != current) {
        std::cerr << "recs: raw files in " << raw_dir.string() << " changed since cache " << cache_file.string()
                  << " was built; rebuilding\n";
      } else {
        return ds;
      }
    } catch (const DatasetError& e) {
      std::cerr << "recs: ignoring unusable cache (" << e.what() << "); rebuilding\n";
    }
  }
  Dataset ds = load_movielens(raw_dir, std::move(name));
  write_cache(ds, cache_file, current);
  return ds;
}

}  // namespace recs
