// Bundle construction and the startup loader (see bundle.hpp).
#include "bundle.hpp"

#include <algorithm>
#include <numeric>
#include <string>

namespace recs::service {

const Model* Bundle::find_model(std::string_view name) const {
  const auto it = models.find(name);
  return it == models.end() ? nullptr : it->second.get();
}

std::shared_ptr<const Bundle> make_bundle(LoadedArtifact&& artifact) {
  auto b = std::make_shared<Bundle>();
  b->manifest = std::move(artifact.manifest);
  b->version = b->manifest.version;
  b->catalog = std::move(artifact.catalog);
  b->models = std::move(artifact.models);

  const Catalog& c = b->catalog;
  const std::size_t n = c.items.size();
  if (c.items.to_dense.size() != n || c.movies.size() != n || c.rating_count.size() != n ||
      c.mean_rating.size() != n || c.popularity_rank.size() != n) {
    throw ArtifactError("bundle: catalog arrays have inconsistent sizes");
  }
  if (b->version.empty()) throw ArtifactError("bundle: empty artifact version");
  if (b->models.empty()) throw ArtifactError("bundle: no models");
  for (const auto& [name, model] : b->models) {
    if (!model || model->n_items() != n) throw ArtifactError("bundle: model " + name + " does not match the catalog");
  }

  // Most rated first. popularity_rank must be a permutation of 1..n.
  b->by_popularity.assign(n, 0);
  std::vector<bool> seen(n, false);
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t r = c.popularity_rank[i];
    if (r == 0 || r > n || seen[r - 1]) throw ArtifactError("bundle: popularity_rank is not a permutation of 1..n");
    seen[r - 1] = true;
    b->by_popularity[r - 1] = static_cast<ItemIdx>(i);
  }
  // Genre bits per item. A catalog genre outside the contract enum (none in MovieLens today) has
  // no bit, so no request can select it.
  b->genre_mask.assign(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::string& g : c.movies[i].genres) {
      if (const auto bit = genre_bit(g)) b->genre_mask[i] |= *bit;
    }
  }
  b->search = SearchIndex(c);
  return b;
}

std::filesystem::path version_dir(const std::filesystem::path& root, const std::string& version) {
  // read_current() already restricts `version` to one safe path component ("../x" and "/abs" are
  // rejected). The directory itself must be a real directory: a symlink could point the service at
  // files outside the artifact root.
  const std::filesystem::path dir = root / version;
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(dir, ec);
  if (ec || std::filesystem::is_symlink(st)) throw ArtifactError("artifact " + version + " is a symlink or unreadable");
  if (!std::filesystem::is_directory(st)) throw ArtifactError("artifact directory not found: " + dir.string());
  return dir;
}

std::shared_ptr<const Bundle> load_current_bundle(const std::filesystem::path& root) {
  std::optional<std::string> version;
  try {
    version = read_current(root);
  } catch (const ArtifactError&) {
    throw;
  } catch (const std::exception& e) {
    throw ArtifactError(std::string("cannot read CURRENT: ") + e.what());
  }
  if (!version) throw ArtifactError("no CURRENT pointer in " + root.string());
  return make_bundle(load_artifact(version_dir(root, *version)));
}

}  // namespace recs::service
