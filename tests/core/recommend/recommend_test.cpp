// Gate 2 (C1): the shared recommend(). A fake model returns fixed scores so the tests control ties
// and -infinity exactly.
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "property.hpp"
#include "recs/model.hpp"

namespace {

constexpr float kNegInf = -std::numeric_limits<float>::infinity();

class FixedScores final : public recs::Model {
 public:
  explicit FixedScores(std::vector<float> s) : s_(std::move(s)) {}
  std::string_view name() const override { return "fixed"; }
  void fit(const recs::Dataset&, const recs::Config&, const recs::TrainContext&) override {}
  recs::UserState fold_in(std::span<const recs::Rating> r) const override {
    recs::UserState u;
    u.ratings.assign(r.begin(), r.end());
    return u;
  }
  void score(const recs::UserState&, std::span<float> out) const override {
    std::copy(s_.begin(), s_.end(), out.begin());
  }
  recs::Explanation explain(const recs::UserState&, recs::ItemIdx) const override { return {}; }
  void save(const recs::fs::path&) const override {}
  void load(const recs::fs::path&) override {}
  std::uint32_t n_items() const override { return static_cast<std::uint32_t>(s_.size()); }
  recs::Config config() const override { return recs::Config::object(); }

 private:
  std::vector<float> s_;
};

recs::UserState rated(std::vector<recs::ItemIdx> items) {
  recs::UserState u;
  for (auto i : items) u.ratings.push_back({i, 4.f, 0});
  return u;
}

std::vector<recs::ItemIdx> items_of(const std::vector<recs::Scored>& v) {
  std::vector<recs::ItemIdx> out;
  for (const auto& s : v) out.push_back(s.item);
  return out;
}

}  // namespace

TEST_CASE("recommend: score descending, ties by ItemIdx ascending", "[c1][recommend]") {
  const FixedScores m({1.f, 3.f, 2.f, 3.f, 1.f});
  const auto r = recs::recommend(m, {}, 5);
  CHECK(items_of(r) == std::vector<recs::ItemIdx>{1, 3, 2, 0, 4});
  CHECK(r[0] == recs::Scored{1, 3.f});
  CHECK(r[2] == recs::Scored{2, 2.f});
}

TEST_CASE("recommend: excludes `exclude` and every rated item", "[c1][recommend]") {
  const FixedScores m({5.f, 4.f, 3.f, 2.f, 1.f});
  const std::vector<recs::ItemIdx> ex = {0, 3};
  CHECK(items_of(recs::recommend(m, rated({1}), 10, ex)) == std::vector<recs::ItemIdx>{2, 4});
  // Out-of-range exclude entries are ignored (e.g. a movie unknown to this artifact).
  const std::vector<recs::ItemIdx> far = {99};
  CHECK(items_of(recs::recommend(m, {}, 2, far)) == std::vector<recs::ItemIdx>{0, 1});
}

TEST_CASE("recommend: never returns -infinity; k = 0 and k > candidates", "[c1][recommend]") {
  const FixedScores m({kNegInf, 1.f, kNegInf, 2.f});
  CHECK(items_of(recs::recommend(m, {}, 10)) == std::vector<recs::ItemIdx>{3, 1});
  CHECK(recs::recommend(m, {}, 0).empty());
  CHECK(items_of(recs::recommend(m, {}, 1)) == std::vector<recs::ItemIdx>{3});
  const FixedScores all_inf({kNegInf, kNegInf});
  CHECK(recs::recommend(all_inf, {}, 5).empty());
  const FixedScores none({});
  CHECK(recs::recommend(none, {}, 5).empty());
}

TEST_CASE("recommend: a NaN score is a model bug and throws", "[c1][recommend]") {
  const FixedScores m({1.f, std::nanf(""), 2.f});
  CHECK_THROWS_AS(recs::recommend(m, {}, 2), std::logic_error);
}

TEST_CASE("recommend matches a brute-force full sort (property)", "[c1][recommend][property]") {
  recs::testing::for_seeds(300, 20261110, [](recs::Rng& rng, std::uint64_t) {
    const auto n = static_cast<std::uint32_t>(rng.uniform(41));
    std::vector<float> s(n);
    for (auto& v : s) {
      // A small value set forces many ties; some items cannot be scored.
      v = rng.uniform(6) == 0 ? kNegInf : static_cast<float>(rng.uniform(5)) - 2.f;
    }
    recs::UserState u;
    std::vector<recs::ItemIdx> ex;
    if (n > 0) {
      for (std::uint64_t i = rng.uniform(n); i > 0; --i) u.ratings.push_back({static_cast<recs::ItemIdx>(rng.uniform(n)), 3.f, 0});
      for (std::uint64_t i = rng.uniform(n); i > 0; --i) ex.push_back(static_cast<recs::ItemIdx>(rng.uniform(n)));
    }
    const std::size_t k = rng.uniform(n + 6);

    std::set<recs::ItemIdx> masked(ex.begin(), ex.end());
    for (const auto& r : u.ratings) masked.insert(r.item);
    std::vector<recs::Scored> expect;
    for (recs::ItemIdx i = 0; i < n; ++i) {
      if (!masked.contains(i) && s[i] != kNegInf) expect.push_back({i, s[i]});
    }
    std::sort(expect.begin(), expect.end(), [](const recs::Scored& a, const recs::Scored& b) {
      return a.score != b.score ? a.score > b.score : a.item < b.item;
    });
    if (expect.size() > k) expect.resize(k);

    const FixedScores m(s);
    CHECK(recs::recommend(m, u, k, ex) == expect);
  });
}
