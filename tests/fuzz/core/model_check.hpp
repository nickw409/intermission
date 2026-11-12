// What the Model::load fuzzers (model_load.hpp) do with a model that loaded: use it once on a tiny
// profile and check every output. Kept free of libFuzzer and file helpers, like score_check.hpp,
// so the unit tests can run it on hand-made models (tests/core/security/fuzz_model_check_test.cpp).
//
// The calls: fold_in() on two ratings (the first and last item), score() (checked before and after
// mask_unrankable(), since the mask overwrites masked items with -inf and would hide a bad score
// from eval's prediction path, which does not mask), explain() for the first and last item, and,
// if the model supports it, similar() for the first and last item (#95). Every score, explanation
// weight and similarity must pass score_allowed(): finite or -infinity, never NaN or +infinity.
// The structural rules of recs/model.hpp are checked as well: explain() cites at most 3 items, all
// of them rated by the user; similar(item, k) returns at most k items, all of them < n_items().
//
// A call that throws a std::exception is a clean refusal, not a failure: the call is skipped.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "recs/model.hpp"
#include "score_check.hpp"

namespace recs::fuzz {

namespace detail {

inline std::string check_scores(const Model& m, const UserState& u) {
  std::vector<float> out(m.n_items());
  m.score(u, out);
  for (float v : out) {
    if (!score_allowed(v)) return "score() gave NaN or +inf";
  }
  m.mask_unrankable(out);
  for (float v : out) {
    if (!score_allowed(v)) return "score() gave NaN or +inf after mask_unrankable()";
  }
  return "";
}

inline std::string check_explain(const Model& m, const UserState& u, ItemIdx item) {
  const Explanation e = m.explain(u, item);
  if (e.because.size() > 3) return "explain() cited more than 3 items";
  for (const Because& b : e.because) {
    if (!score_allowed(b.weight)) return "explain() gave a NaN or +inf weight";
    const bool rated =
        std::any_of(u.ratings.begin(), u.ratings.end(), [&](const Rating& r) { return r.item == b.item; });
    if (!rated) return "explain() cited an item the user did not rate";
  }
  return "";
}

inline std::string check_similar(const Model& m, ItemIdx item, std::size_t k) {
  const std::vector<Scored> s = m.similar(item, k);
  if (s.size() > k) return "similar() returned more than k items";
  for (const Scored& x : s) {
    if (!score_allowed(x.score)) return "similar() gave a NaN or +inf score";
    if (x.item >= m.n_items()) return "similar() returned an unknown item";
  }
  return "";
}

/// Runs one check; a std::exception from the model is a clean refusal and counts as a pass.
template <typename F>
std::string unless_refused(F&& check) {
  try {
    return check();
  } catch (const std::exception&) {
    return "";
  }
}

}  // namespace detail

/// Exercises a loaded model. Returns "" if every output obeys the Model contract, else a short
/// description of the first violation. Models with no items, or more than 2^20, are skipped.
inline std::string check_loaded_model(const Model& m) {
  const std::uint32_t n = m.n_items();
  if (n == 0 || n > (1u << 20)) return "";
  const ItemIdx probes[] = {0, n - 1};
  std::vector<Rating> ratings;
  ratings.push_back({0, 4.5f});
  if (n > 1) ratings.push_back({n - 1, 2.0f});

  std::string bad;
  bool folded = false;
  UserState u;
  try {
    u = m.fold_in(ratings);
    folded = true;
  } catch (const std::exception&) {
    // A model may refuse a profile: then there is nothing to score or explain.
  }
  if (folded) {
    bad = detail::unless_refused([&] { return detail::check_scores(m, u); });
    for (const ItemIdx item : probes) {
      if (bad.empty()) bad = detail::unless_refused([&] { return detail::check_explain(m, u, item); });
    }
  }
  if (m.supports_similar()) {
    for (const ItemIdx item : probes) {
      if (bad.empty()) bad = detail::unless_refused([&] { return detail::check_similar(m, item, 10); });
    }
  }
  return bad;
}

}  // namespace recs::fuzz
