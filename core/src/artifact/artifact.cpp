// Model artifacts: the catalog, write/verify/load, and the CURRENT pointer (contracts/artifact.md).
//
// An artifact is a directory root/<version>/ holding
//   catalog/catalog.bin   the Catalog (format below)
//   <model>/...           whatever Model::save wrote, one directory per model
//   manifest.json         written last; SHA-256 of every other file
//
// Publishing is atomic: everything is written into root/.tmp-<version>-<pid>/, every file and
// directory is fsync'ed, and the directory is renamed to its final name, so a reader never sees a
// half-written artifact under a real version name. A crash leaves only a .tmp-* directory, which
// load_artifact never looks at and the next write_artifact deletes (one writer at a time:
// recs-train runs are queued). The CURRENT pointer is replaced the same way: write
// CURRENT.tmp-<pid>, fsync, rename over CURRENT, fsync the directory.
//
// load_artifact verifies before it loads: the manifest parses and has the known format_version,
// every listed file exists with the listed SHA-256, and no file is present that the manifest does
// not list. Only then are the catalog and models read. Any failure throws ArtifactError, so
// recs-model keeps serving its previous artifact. The manifest itself is not hashed (it holds the
// hashes); a damaged manifest fails to parse or fails one of these checks.
//
// catalog.bin: magic "RECSCAT1", u32 version 1, u32 n_items, then per item:
//   i64 external movie id, i64 movie_id, str title, u32 has_year, i32-as-u32 year,
//   u32 n_genres + n_genres x str, u32 has_imdb, i64 imdb, u32 has_tmdb, i64 tmdb,
//   u32 rating_count, f32 mean_rating, u32 popularity_rank
// where str = u64 byte length + UTF-8 bytes (see artifact/binio.hpp).
#include "recs/artifact.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <numeric>
#include <set>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "artifact/binio.hpp"
#include "recs/sha256.hpp"

namespace recs {
namespace {

constexpr std::string_view kCatalogMagic = "RECSCAT1";
constexpr std::uint32_t kCatalogVersion = 1;
constexpr int kFormatVersion = 1;

// A version (and a model name) is one safe path component: [A-Za-z0-9][A-Za-z0-9._-]{0,99}.
bool valid_component(std::string_view s) {
  if (s.empty() || s.size() > 100) return false;
  if (!std::isalnum(static_cast<unsigned char>(s.front()))) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
  });
}

void check_version(const std::string& version) {
  if (!valid_component(version) || version == "CURRENT") {
    throw std::invalid_argument("invalid artifact version '" + version + "'");
  }
}

void fsync_path(const fs::path& p, bool directory) {
  const int fd = ::open(p.c_str(), O_RDONLY | (directory ? O_DIRECTORY : 0));
  if (fd < 0) throw ArtifactError("open for fsync failed: " + p.string() + ": " + std::strerror(errno));
  const int rc = ::fsync(fd);
  const int err = errno;
  ::close(fd);
  if (rc != 0) throw ArtifactError("fsync failed: " + p.string() + ": " + std::strerror(err));
}

std::string utc_now_iso() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// ---- catalog file -------------------------------------------------------------------------------

void save_catalog(const Catalog& c, const fs::path& file) {
  const auto n = static_cast<std::uint32_t>(c.items.size());
  if (c.movies.size() != n || c.rating_count.size() != n || c.mean_rating.size() != n ||
      c.popularity_rank.size() != n) {
    throw std::invalid_argument("catalog arrays have inconsistent sizes");
  }
  binio::Writer w(file, kCatalogMagic, kCatalogVersion);
  w.u32(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const MovieMeta& m = c.movies[i];
    w.i64(c.items.to_external[i]);
    w.i64(m.movie_id);
    w.str(m.title);
    w.u32(m.year.has_value() ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(m.year.value_or(0)));
    w.u32(static_cast<std::uint32_t>(m.genres.size()));
    for (const auto& g : m.genres) w.str(g);
    w.u32(m.imdb_id.has_value() ? 1 : 0);
    w.i64(m.imdb_id.value_or(0));
    w.u32(m.tmdb_id.has_value() ? 1 : 0);
    w.i64(m.tmdb_id.value_or(0));
    w.u32(c.rating_count[i]);
    w.f32(c.mean_rating[i]);
    w.u32(c.popularity_rank[i]);
  }
  w.close();
}

Catalog load_catalog(const fs::path& file) {
  binio::Reader r(file, kCatalogMagic, kCatalogVersion);
  const std::uint32_t n = r.u32();
  r.need_elems(n, 64);  // lower bound on the bytes per item
  Catalog c;
  c.movies.resize(n);
  c.rating_count.resize(n);
  c.mean_rating.resize(n);
  c.popularity_rank.resize(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int64_t ext = r.i64();
    if (c.items.insert(ext) != i) throw ArtifactError("catalog: duplicate movie id " + std::to_string(ext));
    MovieMeta& m = c.movies[i];
    m.movie_id = r.i64();
    m.title = r.str();
    const bool has_year = r.u32() != 0;
    const auto year = static_cast<int>(static_cast<std::int32_t>(r.u32()));
    if (has_year) m.year = year;
    const std::uint32_t n_genres = r.u32();
    r.need_elems(n_genres, 8);
    for (std::uint32_t g = 0; g < n_genres; ++g) m.genres.push_back(r.str());
    const bool has_imdb = r.u32() != 0;
    const std::int64_t imdb = r.i64();
    if (has_imdb) m.imdb_id = imdb;
    const bool has_tmdb = r.u32() != 0;
    const std::int64_t tmdb = r.i64();
    if (has_tmdb) m.tmdb_id = tmdb;
    c.rating_count[i] = r.u32();
    c.mean_rating[i] = r.f32();
    c.popularity_rank[i] = r.u32();
  }
  r.finish();
  return c;
}

// ---- manifest -----------------------------------------------------------------------------------

nlohmann::json manifest_to_json(const Manifest& m) {
  nlohmann::json files = nlohmann::json::object();
  for (const auto& [path, sha] : m.files) files[path] = sha;
  return {{"format_version", m.format_version}, {"version", m.version},   {"created_at", m.created_at},
          {"git_sha", m.git_sha},               {"dataset", m.dataset},   {"models", m.models},
          {"default_model", m.default_model},   {"files", files},         {"config", m.config}};
}

bool is_sha256_hex(const std::string& s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

Manifest manifest_from_json(const nlohmann::json& j) {
  if (!j.is_object()) throw ArtifactError("manifest: not a JSON object");
  Manifest m;
  try {
    m.format_version = j.at("format_version").get<int>();
    if (m.format_version != kFormatVersion) {
      throw ArtifactError("manifest: unknown format_version " + std::to_string(m.format_version));
    }
    m.version = j.at("version").get<std::string>();
    m.created_at = j.at("created_at").get<std::string>();
    m.git_sha = j.at("git_sha").get<std::string>();
    m.dataset = j.at("dataset").get<std::string>();
    m.models = j.at("models").get<std::vector<std::string>>();
    m.default_model = j.at("default_model").get<std::string>();
    m.config = j.at("config");
    for (const auto& [path, sha] : j.at("files").items()) m.files[path] = sha.get<std::string>();
  } catch (const nlohmann::json::exception& e) {
    throw ArtifactError(std::string("manifest: ") + e.what());
  }
  if (!m.config.is_object()) throw ArtifactError("manifest: config must be an object");
  if (m.models.empty()) throw ArtifactError("manifest: no models");
  std::set<std::string> names;
  for (const auto& name : m.models) {
    if (!valid_component(name) || name == "catalog") throw ArtifactError("manifest: invalid model name '" + name + "'");
    if (!names.insert(name).second) throw ArtifactError("manifest: duplicate model '" + name + "'");
  }
  if (!names.contains(m.default_model)) throw ArtifactError("manifest: default_model is not one of models");
  for (const auto& [path, sha] : m.files) {
    if (!is_sha256_hex(sha)) throw ArtifactError("manifest: bad sha256 for " + path);
    const fs::path p(path);
    if (path.empty() || p.is_absolute() || path.find('\\') != std::string::npos) {
      throw ArtifactError("manifest: bad file path '" + path + "'");
    }
    std::size_t parts = 0;
    for (const auto& part : p) {
      const auto s = part.string();
      if (s.empty() || s == "." || s == "..") throw ArtifactError("manifest: bad file path '" + path + "'");
      ++parts;
    }
    const std::string top = p.begin()->string();
    if (parts < 2 || (top != "catalog" && !names.contains(top))) {
      throw ArtifactError("manifest: file outside catalog/ and the model directories: '" + path + "'");
    }
  }
  return m;
}

std::string read_whole(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw ArtifactError("cannot read " + p.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every regular file under `dir` except the top-level manifest.json, as '/'-separated relative
// paths. Anything that is neither a regular file nor a directory (a symlink, a socket) is rejected.
std::set<std::string> listed_files_on_disk(const fs::path& dir) {
  std::set<std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const auto st = e.symlink_status();
    if (fs::is_directory(st)) continue;
    const std::string rel = e.path().lexically_relative(dir).generic_string();
    if (!fs::is_regular_file(st)) throw ArtifactError("artifact: unexpected non-regular file " + rel);
    if (rel == "manifest.json") continue;
    out.insert(rel);
  }
  return out;
}

}  // namespace

Catalog make_catalog(const Dataset& train) {
  const std::uint32_t n = train.n_items();
  Catalog c;
  c.items = train.items;
  c.movies = train.movies;
  c.rating_count.assign(n, 0);
  c.mean_rating.assign(n, 0.0f);
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto vals = train.by_item.row_values(i);
    double s = 0.0;
    for (float v : vals) s += v;
    c.rating_count[i] = static_cast<std::uint32_t>(vals.size());
    c.mean_rating[i] = vals.empty() ? 0.0f : static_cast<float>(s / static_cast<double>(vals.size()));
  }
  // Rank 1 = most rated; ties broken by lower ItemIdx (same rule as recs::popularity_ranks).
  std::vector<ItemIdx> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(),
                   [&](ItemIdx a, ItemIdx b) { return c.rating_count[a] > c.rating_count[b]; });
  c.popularity_rank.assign(n, 0);
  for (std::uint32_t r = 0; r < n; ++r) c.popularity_rank[order[r]] = r + 1;
  return c;
}

fs::path write_artifact(const fs::path& root, const std::string& version, const Catalog& catalog,
                        const std::vector<const Model*>& models, const std::string& default_model,
                        const std::string& git_sha, const std::string& dataset) {
  check_version(version);
  if (models.empty()) throw std::invalid_argument("write_artifact: no models");
  std::set<std::string> names;
  for (const Model* m : models) {
    if (m == nullptr) throw std::invalid_argument("write_artifact: null model");
    const std::string name(m->name());
    if (!valid_component(name) || name == "catalog") throw std::invalid_argument("write_artifact: bad model name " + name);
    if (!names.insert(name).second) throw std::invalid_argument("write_artifact: duplicate model " + name);
    if (m->n_items() != catalog.items.size()) {
      throw std::invalid_argument("write_artifact: model " + name + " has a different item count than the catalog");
    }
  }
  if (!names.contains(default_model)) throw std::invalid_argument("write_artifact: default_model is not one of models");

  fs::create_directories(root);
  // Remove leftovers of crashed runs (contracts/artifact.md rule 1).
  for (const auto& e : fs::directory_iterator(root)) {
    if (e.path().filename().string().rfind(".tmp-", 0) == 0) fs::remove_all(e.path());
  }
  const fs::path final_dir = root / version;
  if (fs::exists(final_dir)) throw ArtifactError("artifact version already exists: " + final_dir.string());

  const fs::path tmp = root / (".tmp-" + version + "-" + std::to_string(::getpid()));
  fs::create_directories(tmp / "catalog");
  try {
    save_catalog(catalog, tmp / "catalog" / "catalog.bin");
    Manifest man;
    man.version = version;
    man.created_at = utc_now_iso();
    man.git_sha = git_sha;
    man.dataset = dataset;
    man.default_model = default_model;
    man.config = Config::object();
    for (const Model* m : models) {
      const std::string name(m->name());
      fs::create_directory(tmp / name);
      m->save(tmp / name);
      man.models.push_back(name);
      man.config[name] = m->config();
    }
    for (const auto& rel : listed_files_on_disk(tmp)) {
      fsync_path(tmp / rel, false);
      man.files[rel] = sha256_file_hex(tmp / rel);
    }
    {
      std::ofstream out(tmp / "manifest.json", std::ios::binary | std::ios::trunc);
      out << manifest_to_json(man).dump(2) << "\n";
      out.flush();
      if (!out) throw ArtifactError("cannot write manifest.json");
    }
    fsync_path(tmp / "manifest.json", false);
    for (const auto& e : fs::recursive_directory_iterator(tmp)) {
      if (e.is_directory()) fsync_path(e.path(), true);
    }
    fsync_path(tmp, true);
    fs::rename(tmp, final_dir);
    fsync_path(root, true);
  } catch (...) {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    throw;
  }
  return final_dir;
}

LoadedArtifact load_artifact(const fs::path& dir) {
  if (!fs::is_directory(dir)) throw ArtifactError("artifact directory not found: " + dir.string());
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(read_whole(dir / "manifest.json"));
  } catch (const nlohmann::json::exception& e) {
    throw ArtifactError(std::string("manifest.json does not parse: ") + e.what());
  }
  LoadedArtifact out;
  out.manifest = manifest_from_json(j);
  const Manifest& man = out.manifest;

  std::set<std::string> on_disk;
  try {
    on_disk = listed_files_on_disk(dir);
  } catch (const fs::filesystem_error& e) {
    throw ArtifactError(std::string("artifact: ") + e.what());
  }
  for (const auto& [path, sha] : man.files) {
    if (!on_disk.contains(path)) throw ArtifactError("artifact: missing file " + path);
  }
  for (const auto& path : on_disk) {
    if (!man.files.contains(path)) throw ArtifactError("artifact: unlisted file " + path);
  }
  for (const auto& [path, sha] : man.files) {
    std::string actual;
    try {
      actual = sha256_file_hex(dir / path);
    } catch (const std::exception& e) {
      throw ArtifactError("artifact: cannot hash " + path + ": " + e.what());
    }
    if (actual != sha) throw ArtifactError("artifact: checksum mismatch for " + path);
  }

  // Verified: now load.
  try {
    out.catalog = load_catalog(dir / "catalog" / "catalog.bin");
    for (const auto& name : man.models) {
      auto model = make_model(name);
      model->load(dir / name);
      if (model->n_items() != out.catalog.items.size()) {
        throw ArtifactError("model " + name + " has a different item count than the catalog");
      }
      out.models.emplace(name, std::move(model));
    }
  } catch (const ArtifactError&) {
    throw;
  } catch (const std::exception& e) {
    throw ArtifactError(std::string("artifact: load failed: ") + e.what());
  }
  return out;
}

void write_current(const fs::path& root, const std::string& version) {
  check_version(version);
  fs::create_directories(root);
  const fs::path tmp = root / ("CURRENT.tmp-" + std::to_string(::getpid()));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << version << "\n";
    out.flush();
    if (!out) throw ArtifactError("cannot write " + tmp.string());
  }
  fsync_path(tmp, false);
  fs::rename(tmp, root / "CURRENT");
  fsync_path(root, true);
}

std::optional<std::string> read_current(const fs::path& root) {
  const fs::path file = root / "CURRENT";
  if (!fs::exists(file)) return std::nullopt;
  std::string s = read_whole(file);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  if (!valid_component(s) || s == "CURRENT") throw ArtifactError("CURRENT holds an invalid version");
  return s;
}

}  // namespace recs
