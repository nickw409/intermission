// A small, valid artifact for the artifact fuzzers and the seed generator: three users, four
// items, models "pop" and "pop_count". Built once per process under the scratch directory.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fuzz_util.hpp"
#include "recs/artifact.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace recs::fuzz {

inline constexpr const char* kTemplateVersion = "v1";

inline Dataset template_dataset() {
  std::vector<MovieMeta> movies(4);
  movies[0] = {101, "Toy Story", 1995, {"Animation", "Comedy"}, 114709, 862};
  movies[1] = {205, "Amelie", 2001, {"Romance"}, 211915, std::nullopt};
  movies[2] = {7, "Hamlet", std::nullopt, {}, std::nullopt, std::nullopt};
  movies[3] = {99999, "Never Rated", 2020, {"Drama"}, std::nullopt, 5};
  return make_dataset("fuzz-fixture", 3, 4,
                      {{0, 0, 5.f, 1}, {0, 1, 3.f, 2}, {1, 0, 4.f, 3}, {1, 2, 2.f, 4}, {2, 0, 3.f, 5}, {2, 1, 4.f, 6}},
                      movies);
}

/// Writes the template artifact into `root`/v1 and returns that directory.
inline fs::path write_template_artifact(const fs::path& root) {
  const Dataset ds = template_dataset();
  auto pop = make_popularity();
  auto count = make_popularity_count();
  pop->fit(ds, Config{{"m", 5}}, {});
  count->fit(ds, Config::object(), {});
  return write_artifact(root, kTemplateVersion, make_catalog(ds), {pop.get(), count.get()}, "pop", "abc1234",
                        "fuzz-fixture");
}

/// The per-process template artifact directory (created on first use).
inline const fs::path& template_artifact() {
  static const fs::path dir = write_template_artifact(fresh_dir("artifact-root"));
  return dir;
}

}  // namespace recs::fuzz
