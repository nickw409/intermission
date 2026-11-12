// Which scores a model may produce, as checked by the Model::load fuzzers (model_load.hpp) on
// every score, explanation weight and similarity (model_check.hpp). Kept free of libFuzzer and
// file helpers so the unit tests can include it (tests/core/security/fuzz_score_check_test.cpp).
//
// The Model contract (recs/model.hpp) forbids NaN and uses -infinity for "cannot score" and for
// masked items, so -infinity is the only legal non-finite score. +infinity means the model's state
// is corrupt; recs-model would drop those items (it never emits non-finite scores), so the items a
// model ranks highest would silently disappear from recommendations.
#pragma once

#include <cmath>
#include <limits>

namespace recs::fuzz {

inline bool score_allowed(float v) {
  return !std::isnan(v) && v != std::numeric_limits<float>::infinity();
}

}  // namespace recs::fuzz
