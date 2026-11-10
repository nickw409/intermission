// Model artifacts and the CURRENT pointer (contracts/artifact.md). Workstream C owns the
// implementation. FROZEN after Wave 0.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "recs/model.hpp"
#include "recs/types.hpp"

namespace recs {

namespace fs = std::filesystem;

class ArtifactError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// What recs-model needs about the catalog, indexed by ItemIdx.
struct Catalog {
  IdMap items;
  std::vector<MovieMeta> movies;
  std::vector<std::uint32_t> rating_count;
  std::vector<float> mean_rating;               // 0 when rating_count == 0
  std::vector<std::uint32_t> popularity_rank;   // 1 = most rated
};

Catalog make_catalog(const Dataset& train);

struct Manifest {
  int format_version = 1;
  std::string version;     // e.g. "2026-11-09T21-14-03Z"
  std::string created_at;  // ISO 8601 UTC
  std::string git_sha;
  std::string dataset;
  std::vector<std::string> models;
  std::string default_model;  // served by For you; must be one of `models`
  std::map<std::string, std::string> files;  // path relative to the artifact dir -> sha256 hex
  Config config;                             // per-model configs, keyed by model name
};

/// Layout and rules: contracts/artifact.md.
/// Writes root/<version>/ via a temporary directory + rename, so a partial artifact is never
/// visible under its final name. Each model saves into root/<version>/<model name>/. The manifest
/// is written last and lists the SHA-256 of every other file. Returns the final directory.
fs::path write_artifact(const fs::path& root, const std::string& version, const Catalog& catalog,
                        const std::vector<const Model*>& models, const std::string& default_model,
                        const std::string& git_sha, const std::string& dataset);

struct LoadedArtifact {
  Manifest manifest;
  Catalog catalog;
  std::map<std::string, std::unique_ptr<Model>, std::less<>> models;
};

/// Verifies every checksum in the manifest before loading anything; throws ArtifactError on any
/// missing, extra-unlisted, or mismatching file.
LoadedArtifact load_artifact(const fs::path& dir);

/// Atomically points root/CURRENT at `version` (write temp file, fsync, rename).
void write_current(const fs::path& root, const std::string& version);
std::optional<std::string> read_current(const fs::path& root);

}  // namespace recs
