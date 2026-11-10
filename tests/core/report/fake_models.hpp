// Test doubles for evaluator tests: models with fixed, user-independent scores.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "recs/model.hpp"

namespace recs::testing {

/// Scores item i with scores[i] for every user. Optionally predicts ratings and/or exposes a
/// batch path that, for users in `broken_trained_users`, scores every item -infinity.
class FixedScoreModel final : public Model {
 public:
  explicit FixedScoreModel(std::vector<float> scores, bool predicts = false, bool trained = false,
                           std::vector<UserIdx> broken_trained_users = {})
      : s_(std::move(scores)), predicts_(predicts), trained_(trained), broken_(std::move(broken_trained_users)) {}

  std::string_view name() const override { return "fixed"; }
  void fit(const Dataset&, const Config&, const TrainContext&) override {}
  UserState fold_in(std::span<const Rating> r) const override {
    UserState u;
    u.ratings.assign(r.begin(), r.end());
    return u;
  }
  void score(const UserState&, std::span<float> out) const override { std::copy(s_.begin(), s_.end(), out.begin()); }
  Explanation explain(const UserState&, ItemIdx) const override { return {}; }
  void save(const fs::path&) const override {}
  void load(const fs::path&) override {}
  std::uint32_t n_items() const override { return static_cast<std::uint32_t>(s_.size()); }
  Config config() const override { return Config{{"fixed", true}}; }
  bool predicts_ratings() const override { return predicts_; }
  bool supports_trained_scoring() const override { return trained_; }
  void score_trained_user(UserIdx u, std::span<float> out) const override {
    if (std::find(broken_.begin(), broken_.end(), u) != broken_.end()) {
      std::fill(out.begin(), out.end(), -std::numeric_limits<float>::infinity());
    } else {
      std::copy(s_.begin(), s_.end(), out.begin());
    }
  }

 private:
  std::vector<float> s_;
  bool predicts_;
  bool trained_;
  std::vector<UserIdx> broken_;
};

/// A rating model with fractional, user-independent scores (score_i = 1 + (0.37 * i mod 4)) and a
/// batch path that disagrees with fold-in on a user-dependent subset of items, so per-user parity
/// Jaccards and rating errors are fractional. Used by thread-count determinism tests.
class PerturbedModel final : public Model {
 public:
  explicit PerturbedModel(std::uint32_t n_items) : n_(n_items) {}
  std::string_view name() const override { return "perturbed"; }
  void fit(const Dataset&, const Config&, const TrainContext&) override {}
  UserState fold_in(std::span<const Rating> r) const override {
    UserState u;
    u.ratings.assign(r.begin(), r.end());
    return u;
  }
  void score(const UserState&, std::span<float> out) const override {
    for (std::uint32_t i = 0; i < n_; ++i) out[i] = base(i);
  }
  Explanation explain(const UserState&, ItemIdx) const override { return {}; }
  void save(const fs::path&) const override {}
  void load(const fs::path&) override {}
  std::uint32_t n_items() const override { return n_; }
  Config config() const override { return Config::object(); }
  bool predicts_ratings() const override { return true; }
  bool supports_trained_scoring() const override { return true; }
  void score_trained_user(UserIdx u, std::span<float> out) const override {
    for (std::uint32_t i = 0; i < n_; ++i) {
      out[i] = base(i) - (((i * 7 + u) % 5 == 0) ? 0.9f + 0.013f * static_cast<float>(u % 11) : 0.f);
    }
  }

 private:
  static float base(std::uint32_t i) { return 1.f + std::fmod(0.37f * static_cast<float>(i), 4.f); }
  std::uint32_t n_;
};

}  // namespace recs::testing
