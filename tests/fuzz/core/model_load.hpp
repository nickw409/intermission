// Model::load harness body, shared by the per-model fuzz targets (model_load_<name>.cpp).
//
// Each model saves one binary file, plus a config.json for the models whose binary layout depends
// on their config. For those, the fuzz input is split with split_config() (u16 length, config.json,
// binary file); otherwise the whole input is the binary file. content's features.bin ends in a
// SHA-256, which the harness appends (see fuzz_util.hpp).
//
// After a successful load the model is used once (fold_in + score over a few items), so a file
// that loads but leaves inconsistent state is caught too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_util.hpp"
#include "recs/model.hpp"

namespace recs::fuzz {

struct ModelFiles {
  const char* binary;  // file name of the model's binary file
  bool has_config;     // also writes config.json from the split input
  bool sealed;         // binary ends in SHA-256(body)
};

inline ModelFiles model_files(std::string_view model) {
  if (model == "pop" || model == "pop_count") return {"scores.bin", false, false};
  if (model == "knn") return {"item_knn.bin", false, false};
  if (model == "content") return {"features.bin", true, true};
  if (model == "mf") return {"item_factors.bin", true, false};
  if (model == "als") return {"factors.bin", true, false};
  __builtin_trap();  // a harness names an unknown model
}

/// Writes `input` into `dir` in the layout model_files(model) describes.
inline void write_model_input(std::string_view model, std::string_view input, const fs::path& dir) {
  const ModelFiles f = model_files(model);
  std::string_view binary = input;
  if (f.has_config) {
    const SplitInput s = split_config(input);
    write_file(dir / "config.json", s.config);
    binary = s.binary;
  }
  write_file(dir / f.binary, f.sealed ? sealed(binary) : std::string(binary));
}

inline int model_load(std::string_view model, const std::uint8_t* data, std::size_t size) {
  const fs::path dir = fresh_dir(model);
  write_model_input(model, as_view(data, size), dir);
  auto m = make_model(model);
  try {
    m->load(dir);
  } catch (const std::exception&) {
    return 0;
  }
  // Loaded: the model must be usable. Exercise fold-in and scoring on a tiny profile.
  const std::uint32_t n = m->n_items();
  if (n == 0 || n > (1u << 20)) return 0;
  std::vector<Rating> ratings;
  ratings.push_back({0, 4.5f});
  if (n > 1) ratings.push_back({n - 1, 2.0f});
  try {
    const UserState u = m->fold_in(ratings);
    std::vector<float> out(n);
    m->score(u, out);
    for (float v : out) {
      if (v != v) __builtin_trap();  // NaN scores are forbidden by the Model contract
    }
  } catch (const std::exception&) {
    // A model may refuse a profile; that is a clean failure, not a crash.
  }
  return 0;
}

}  // namespace recs::fuzz
