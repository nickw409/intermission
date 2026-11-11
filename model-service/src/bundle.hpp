// A Bundle is one loaded, verified artifact plus the serving indexes built from it. It is
// immutable after construction and shared by reference count: every request takes one snapshot
// (a std::shared_ptr<const Bundle>) at its start and uses only that snapshot, so a hot swap never
// mixes two artifact versions inside one response, and an old bundle is freed when its last
// in-flight request finishes.
//
// The swap itself (watching CURRENT, verifying and loading off the request path) belongs to
// workstream P (model-service/swap/). This file provides the holder P swaps into and the one-shot
// loader used at startup.
#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "genres.hpp"
#include "recs/artifact.hpp"
#include "search.hpp"

namespace recs::service {

struct Bundle {
  std::string version;  // manifest.version; sent as X-Model-Version
  Manifest manifest;
  Catalog catalog;
  std::map<std::string, std::unique_ptr<Model>, std::less<>> models;
  SearchIndex search;
  std::vector<ItemIdx> by_popularity;  // every item, most rated first (catalog.popularity_rank)
  std::vector<GenreMask> genre_mask;   // per item: its genres as bits (genres.hpp); 0 = none listed

  /// The loaded model with this name, or nullptr.
  const Model* find_model(std::string_view name) const;
  std::uint32_t n_items() const { return catalog.items.size(); }
};

/// Builds the serving indexes and checks the artifact is internally consistent (catalog arrays
/// agree in size, every model scores the catalog's items, popularity ranks are 1..n). Throws
/// ArtifactError otherwise, so an inconsistent artifact is never served.
std::shared_ptr<const Bundle> make_bundle(LoadedArtifact&& artifact);

/// root/<version> after checking it is a real directory, not a symlink. Throws ArtifactError.
std::filesystem::path version_dir(const std::filesystem::path& root, const std::string& version);

/// Reads root/CURRENT, verifies and loads that artifact, and builds a Bundle. Throws ArtifactError
/// if CURRENT is missing or the artifact fails verification.
std::shared_ptr<const Bundle> load_current_bundle(const std::filesystem::path& root);

/// The currently serving bundle. Readers and the (future) swapper use atomic shared_ptr
/// operations, so a swap is wait-free for readers and never tears.
class BundleHolder {
 public:
  std::shared_ptr<const Bundle> get() const { return current_.load(std::memory_order_acquire); }
  void store(std::shared_ptr<const Bundle> b) { current_.store(std::move(b), std::memory_order_release); }

 private:
  std::atomic<std::shared_ptr<const Bundle>> current_;
};

}  // namespace recs::service
