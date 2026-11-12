// Gate for the score check the Model::load fuzzers apply after fold_in + score + mask_unrankable
// (tests/fuzz/core/model_load.hpp). The Model contract (recs/model.hpp) forbids NaN and reserves
// -infinity for "cannot score" / masked items, so the only legal non-finite score is -infinity.
// +infinity is a corrupt model: recs-model drops non-finite scores before responding, so the
// model's "best" items would silently vanish from recommendations.
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "../../fuzz/core/score_check.hpp"

using recs::fuzz::score_allowed;

TEST_CASE("fuzz score check accepts finite scores and -infinity", "[security][fuzz]") {
  constexpr float kInf = std::numeric_limits<float>::infinity();
  CHECK(score_allowed(0.0f));
  CHECK(score_allowed(-0.0f));
  CHECK(score_allowed(4.5f));
  CHECK(score_allowed(-1e30f));
  CHECK(score_allowed(std::numeric_limits<float>::max()));
  CHECK(score_allowed(std::numeric_limits<float>::lowest()));
  CHECK(score_allowed(std::numeric_limits<float>::denorm_min()));
  CHECK(score_allowed(-kInf));  // "cannot score" and mask_unrankable() use -infinity
}

TEST_CASE("fuzz score check rejects NaN and +infinity", "[security][fuzz]") {
  constexpr float kInf = std::numeric_limits<float>::infinity();
  CHECK_FALSE(score_allowed(kInf));
  CHECK_FALSE(score_allowed(std::numeric_limits<float>::quiet_NaN()));
  CHECK_FALSE(score_allowed(-std::numeric_limits<float>::quiet_NaN()));
  CHECK_FALSE(score_allowed(std::numeric_limits<float>::signaling_NaN()));
  CHECK_FALSE(score_allowed(std::numeric_limits<float>::max() * 2.0f));  // overflow to +inf
}
