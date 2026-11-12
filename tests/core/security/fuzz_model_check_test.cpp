// Gate for what the Model::load fuzzers do with a model that loaded (tests/fuzz/core/model_check.hpp,
// #95): besides fold_in + score, they call similar() and explain(), and every score or weight those
// return must pass score_allowed() (finite or -infinity). A file that loads but breaks only the
// similar-items or the explanation path must be caught too.
#include <cmath>
#include <limits>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../fuzz/core/model_check.hpp"
#include "recs/model.hpp"

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// A model whose outputs are set by the test. By default everything obeys the contract.
class FakeModel final : public recs::Model {
 public:
  float score_value = 1.0f;
  float similar_score = 0.5f;
  float because_weight = 0.25f;
  bool has_similar = true;
  bool similar_throws = false;
  bool explain_throws = false;
  recs::ItemIdx similar_item = 1;  // the neighbour similar() returns
  recs::ItemIdx because_item = 0;  // the item explain() cites (must be one the user rated)
  std::size_t similar_extra = 0;   // similar() returns k + this many entries
  std::size_t because_count = 1;

  std::string_view name() const override { return "fake"; }
  void fit(const recs::Dataset&, const recs::Config&, const recs::TrainContext&) override {}
  recs::UserState fold_in(std::span<const recs::Rating> r) const override {
    recs::UserState u;
    u.ratings.assign(r.begin(), r.end());
    return u;
  }
  void score(const recs::UserState&, std::span<float> out) const override {
    for (float& v : out) v = score_value;
  }
  recs::Explanation explain(const recs::UserState&, recs::ItemIdx) const override {
    if (explain_throws) throw std::invalid_argument("fake: no explanation");
    recs::Explanation e;
    for (std::size_t k = 0; k < because_count; ++k) e.because.push_back({because_item, because_weight});
    return e;
  }
  void save(const recs::fs::path&) const override {}
  void load(const recs::fs::path&) override {}
  std::uint32_t n_items() const override { return 5; }
  recs::Config config() const override { return recs::Config::object(); }
  bool supports_similar() const override { return has_similar; }
  std::vector<recs::Scored> similar(recs::ItemIdx, std::size_t k) const override {
    if (!has_similar) return Model::similar(0, k);
    if (similar_throws) throw std::invalid_argument("fake: no neighbours");
    return std::vector<recs::Scored>(k + similar_extra, recs::Scored{similar_item, similar_score});
  }
};

using recs::fuzz::check_loaded_model;

}  // namespace

TEST_CASE("fuzz model check passes a model that obeys the contract", "[security][fuzz]") {
  FakeModel m;
  CHECK(check_loaded_model(m).empty());
  m.score_value = -kInf;  // "cannot score"
  m.similar_score = -kInf;
  m.because_weight = -1.0f;
  CHECK(check_loaded_model(m).empty());
  m.has_similar = false;  // similar() is only called on models that support it
  CHECK(check_loaded_model(m).empty());
  m.has_similar = true;
  m.similar_throws = true;  // refusing is clean
  m.explain_throws = true;
  CHECK(check_loaded_model(m).empty());
  m = FakeModel{};
  m.because_count = 0;  // an empty explanation is allowed
  CHECK(check_loaded_model(m).empty());
}

TEST_CASE("fuzz model check traps NaN or +inf from score()", "[security][fuzz]") {
  for (const float v : {kNaN, kInf}) {
    FakeModel m;
    m.score_value = v;
    CHECK_FALSE(check_loaded_model(m).empty());
  }
}

TEST_CASE("fuzz model check traps NaN or +inf from similar()", "[security][fuzz]") {
  for (const float v : {kNaN, -kNaN, kInf}) {
    FakeModel m;
    m.similar_score = v;
    CHECK_FALSE(check_loaded_model(m).empty());
  }
}

TEST_CASE("fuzz model check traps similar() results that break the contract", "[security][fuzz]") {
  FakeModel m;
  m.similar_item = 5;  // == n_items: out of range
  CHECK_FALSE(check_loaded_model(m).empty());
  m = FakeModel{};
  m.similar_extra = 1;  // more than k results
  CHECK_FALSE(check_loaded_model(m).empty());
}

TEST_CASE("fuzz model check traps NaN or +inf from explain()", "[security][fuzz]") {
  for (const float v : {kNaN, -kNaN, kInf}) {
    FakeModel m;
    m.because_weight = v;
    CHECK_FALSE(check_loaded_model(m).empty());
  }
}

TEST_CASE("fuzz model check traps explanations that break the contract", "[security][fuzz]") {
  FakeModel m;
  m.because_item = 2;  // the profile rates items 0 and 4 only
  CHECK_FALSE(check_loaded_model(m).empty());
  m = FakeModel{};
  m.because_count = 4;  // at most 3 entries
  CHECK_FALSE(check_loaded_model(m).empty());
}
