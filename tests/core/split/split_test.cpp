// Gate tests for recs/split.hpp (B1 gate 1): the temporal train / validation / test split.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <omp.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "property.hpp"
#include "recs/dataset.hpp"
#include "recs/rng.hpp"
#include "recs/sha256.hpp"
#include "recs/sparse.hpp"
#include "recs/split.hpp"

using recs::Dataset;
using recs::ItemIdx;
using recs::Rating;
using recs::Split;
using recs::SplitConfig;
using recs::Triplet;
using recs::UserIdx;

namespace {

// ---------------------------------------------------------------------------------------------
// Reference rules, written independently of the implementation from the header text.

std::uint32_t round_half_up(double x) { return static_cast<std::uint32_t>(std::floor(x + 0.5)); }

struct Sizes {
  std::uint32_t n_test = 0, n_val = 0;
  bool feasible = true;  // false if the clamped sizes do not fit in the user's ratings
};

Sizes expected_sizes(std::uint32_t n, const SplitConfig& c) {
  Sizes s;
  s.n_test = std::clamp(round_half_up(c.test_fraction * n), c.test_min, c.test_max);
  if (s.n_test > n) return {s.n_test, 0, false};
  const std::uint32_t n_train = n - s.n_test;
  s.n_val = std::clamp(round_half_up(c.val_fraction * n_train), c.val_min, c.val_max);
  if (s.n_val > n_train) s.feasible = false;
  return s;
}

// The sort key of the header: (timestamp, tie_key = mix_seed(seed, user, item)). The item is a
// last resort for 64-bit key collisions only.
using Key = std::tuple<std::int64_t, std::uint64_t, ItemIdx>;
Key key_of(const SplitConfig& c, UserIdx u, ItemIdx item, std::int64_t ts) {
  return {ts, recs::mix_seed(c.seed, u, item), item};
}

std::string config_json(const SplitConfig& c) {
  nlohmann::json j;
  j["min_ratings"] = c.min_ratings;
  j["seed"] = c.seed;
  j["test_fraction"] = c.test_fraction;
  j["test_max"] = c.test_max;
  j["test_min"] = c.test_min;
  j["val_fraction"] = c.val_fraction;
  j["val_max"] = c.val_max;
  j["val_min"] = c.val_min;
  return j.dump();
}

std::vector<Rating> row_ratings(const recs::Csr& m, std::uint32_t r) {
  std::vector<Rating> out;
  const auto idx = m.row_indices(r);
  const auto val = m.row_values(r);
  const auto ts = m.row_ts(r);
  for (std::size_t i = 0; i < idx.size(); ++i) out.push_back(Rating{idx[i], val[i], ts[i]});
  return out;
}

bool same_dataset(const Dataset& a, const Dataset& b) {
  return a.name == b.name && a.users == b.users && a.items == b.items && a.by_user == b.by_user &&
         a.by_item == b.by_item && a.movies == b.movies && a.tags == b.tags && a.genome == b.genome;
}

bool same_split(const Split& a, const Split& b) {
  return a.hash == b.hash && same_dataset(a.train, b.train) &&
         same_dataset(a.train_minus_val, b.train_minus_val) && a.validation == b.validation &&
         a.test == b.test && a.eval_users == b.eval_users && a.cold_item_share == b.cold_item_share;
}

// ---------------------------------------------------------------------------------------------
// Full invariant check of one split against its input.
void check_split(const Dataset& full, const SplitConfig& c, const Split& sp) {
  const std::uint32_t n_users = full.n_users();

  // Config is recorded verbatim.
  CHECK(sp.config.min_ratings == c.min_ratings);
  CHECK(sp.config.seed == c.seed);
  CHECK(sp.config.test_fraction == c.test_fraction);
  CHECK(sp.config.val_fraction == c.val_fraction);
  CHECK(sp.config.test_min == c.test_min);
  CHECK(sp.config.test_max == c.test_max);
  CHECK(sp.config.val_min == c.val_min);
  CHECK(sp.config.val_max == c.val_max);

  // Index spaces and metadata are kept; by_item is the transpose of by_user.
  for (const Dataset* d : {&sp.train, &sp.train_minus_val}) {
    REQUIRE(d->n_users() == n_users);
    REQUIRE(d->n_items() == full.n_items());
    CHECK(d->users == full.users);
    CHECK(d->items == full.items);
    CHECK(d->movies == full.movies);
    REQUIRE_NOTHROW(recs::validate(d->by_user));
    CHECK(d->by_user.n_rows == n_users);
    CHECK(d->by_user.n_cols == full.n_items());
    CHECK(d->by_item == recs::transpose(d->by_user));
  }
  REQUIRE(sp.validation.size() == n_users);
  REQUIRE(sp.test.size() == n_users);

  std::vector<UserIdx> expected_eval;
  std::string hash_input = config_json(c);
  std::uint64_t n_test_total = 0;

  for (UserIdx u = 0; u < n_users; ++u) {
    INFO("user " << u);
    const auto all = row_ratings(full.by_user, u);
    const auto train = row_ratings(sp.train.by_user, u);
    const auto tmv = row_ratings(sp.train_minus_val.by_user, u);
    const auto& val = sp.validation[u];
    const auto& test = sp.test[u];
    const auto n = static_cast<std::uint32_t>(all.size());

    // Held-out vectors are in ascending item order.
    CHECK(std::is_sorted(val.begin(), val.end(), [](auto& a, auto& b) { return a.item < b.item; }));
    CHECK(std::is_sorted(test.begin(), test.end(), [](auto& a, auto& b) { return a.item < b.item; }));

    if (n < c.min_ratings) {
      // Not evaluated: everything stays in train and train_minus_val.
      CHECK(train == all);
      CHECK(tmv == all);
      CHECK(val.empty());
      CHECK(test.empty());
      for (const auto& r : all) hash_input += std::to_string(u) + "," + std::to_string(r.item) + ",t\n";
      continue;
    }
    expected_eval.push_back(u);
    const Sizes s = expected_sizes(n, c);
    REQUIRE(s.feasible);
    CHECK(test.size() == s.n_test);
    CHECK(val.size() == s.n_val);
    CHECK(train.size() == n - s.n_test);
    CHECK(tmv.size() == n - s.n_test - s.n_val);
    n_test_total += test.size();

    // Partition: tmv, val, test are disjoint by item; tmv u val = train; train u test = all.
    std::map<ItemIdx, char> where;
    auto put = [&](const std::vector<Rating>& xs, char set) {
      for (const auto& r : xs) {
        INFO("item " << r.item << " set " << set);
        CHECK(where.emplace(r.item, set).second);  // no (user, item) in two sets
      }
    };
    put(tmv, 't');
    put(val, 'v');
    put(test, 'x');
    std::vector<Rating> tmv_val = tmv;
    tmv_val.insert(tmv_val.end(), val.begin(), val.end());
    std::sort(tmv_val.begin(), tmv_val.end(), [](auto& a, auto& b) { return a.item < b.item; });
    CHECK(tmv_val == train);
    std::vector<Rating> train_test = train;
    train_test.insert(train_test.end(), test.begin(), test.end());
    std::sort(train_test.begin(), train_test.end(), [](auto& a, auto& b) { return a.item < b.item; });
    CHECK(train_test == all);  // also checks values and timestamps are carried unchanged

    // Temporal order (the spec's invariant), stated on timestamps:
    // every test ts >= every train ts; every val ts >= every tmv ts and <= every test ts.
    // "every a >= every b" is checked as min(a) >= max(b).
    auto min_ts = [](const std::vector<Rating>& xs) {
      std::int64_t m = std::numeric_limits<std::int64_t>::max();
      for (const auto& r : xs) m = std::min(m, r.ts);
      return m;
    };
    auto max_ts = [](const std::vector<Rating>& xs) {
      std::int64_t m = std::numeric_limits<std::int64_t>::min();
      for (const auto& r : xs) m = std::max(m, r.ts);
      return m;
    };
    if (!test.empty() && !train.empty()) CHECK(min_ts(test) >= max_ts(train));
    if (!val.empty() && !tmv.empty()) CHECK(min_ts(val) >= max_ts(tmv));
    if (!val.empty() && !test.empty()) CHECK(max_ts(val) <= min_ts(test));

    // The exact rule: sorted by (ts, mix_seed(seed, user, item)), test is the tail, val the tail
    // of the rest. So every key in an earlier set is strictly below every key in a later set,
    // i.e. the largest key of the earlier set < the smallest key of the later set.
    auto before = [&](const std::vector<Rating>& lo, const std::vector<Rating>& hi) {
      if (lo.empty() || hi.empty()) return;
      Key max_lo = key_of(c, u, lo[0].item, lo[0].ts);
      for (const auto& a : lo) max_lo = std::max(max_lo, key_of(c, u, a.item, a.ts));
      Key min_hi = key_of(c, u, hi[0].item, hi[0].ts);
      for (const auto& b : hi) min_hi = std::min(min_hi, key_of(c, u, b.item, b.ts));
      CHECK(max_lo < min_hi);
    };
    before(tmv, val);
    before(tmv, test);
    before(val, test);

    for (const auto& r : all) {
      hash_input += std::to_string(u) + "," + std::to_string(r.item) + "," + where.at(r.item) + "\n";
    }
  }
  CHECK(sp.eval_users == expected_eval);
  CHECK(sp.hash == recs::sha256_hex(hash_input));

  // Tags (leakage guard): drop a tag whose (tagging user, item) is held out of that Dataset.
  auto held_in = [&](const std::vector<std::vector<Rating>>& sets, const recs::TagApplication& t) {
    const auto u = full.users.find(t.user_id);
    if (!u) return false;
    for (const auto& r : sets[*u])
      if (r.item == t.item) return true;
    return false;
  };
  std::vector<recs::TagApplication> want_train, want_tmv;
  for (const auto& t : full.tags) {
    if (!held_in(sp.test, t)) want_train.push_back(t);
    if (!held_in(sp.test, t) && !held_in(sp.validation, t)) want_tmv.push_back(t);
  }
  CHECK(sp.train.tags == want_train);
  CHECK(sp.train_minus_val.tags == want_tmv);

  // cold_item_share by brute force: test ratings whose item has no rating in train.
  std::set<ItemIdx> train_items;
  for (UserIdx u = 0; u < n_users; ++u)
    for (auto i : sp.train.by_user.row_indices(u)) train_items.insert(i);
  std::uint64_t cold = 0;
  for (UserIdx u = 0; u < n_users; ++u)
    for (const auto& r : sp.test[u]) cold += train_items.count(r.item) == 0 ? 1 : 0;
  const double expected_cold =
      n_test_total == 0 ? 0.0 : static_cast<double>(cold) / static_cast<double>(n_test_total);
  CHECK(sp.cold_item_share == expected_cold);
}

// Random dataset with many timestamp ties: timestamps are drawn from a tiny range.
Dataset random_dataset(recs::Rng& rng) {
  const auto n_users = static_cast<std::uint32_t>(1 + rng.uniform(30));
  const auto n_items = static_cast<std::uint32_t>(1 + rng.uniform(40));
  const std::uint64_t ts_choices[] = {1, 2, 3, 5, 1000};
  const std::uint64_t ts_range = ts_choices[rng.uniform(5)];
  std::vector<Triplet> t;
  std::vector<ItemIdx> items(n_items);
  for (ItemIdx i = 0; i < n_items; ++i) items[i] = i;
  for (UserIdx u = 0; u < n_users; ++u) {
    rng.shuffle(items.begin(), items.end());
    const auto count = rng.uniform(std::min<std::uint64_t>(n_items, 36) + 1);
    for (std::uint64_t j = 0; j < count; ++j) {
      const float value = 0.5f * static_cast<float>(1 + rng.uniform(10));
      const auto ts = static_cast<std::int64_t>(1'000'000 + rng.uniform(ts_range));
      t.push_back({u, items[j], value, ts});
    }
  }
  Dataset ds = recs::make_dataset("random", n_users, n_items, std::move(t));
  // Tags: mostly by rating users on items they rated (so some land on held-out pairs), some on
  // unrated items, and some by external users who have no ratings at all.
  const auto n_tags = rng.uniform(40);
  for (std::uint64_t j = 0; j < n_tags; ++j) {
    const auto user = static_cast<std::int64_t>(rng.uniform(n_users + 3));
    const auto item = static_cast<ItemIdx>(rng.uniform(n_items));
    ds.tags.push_back({user, item, "tag" + std::to_string(j), 0});
    if (user < static_cast<std::int64_t>(n_users)) {
      const auto row = ds.by_user.row_indices(static_cast<UserIdx>(user));
      if (!row.empty()) {
        ds.tags.push_back({user, row[rng.uniform(row.size())], "rated" + std::to_string(j), 0});
      }
    }
  }
  return ds;
}

SplitConfig random_config(recs::Rng& rng) {
  SplitConfig c;
  c.seed = rng.next_u64();
  if (rng.uniform(2) == 0) return c;  // half the cases: the production config, new seed
  c.min_ratings = static_cast<std::uint32_t>(1 + rng.uniform(12));
  c.test_fraction = static_cast<double>(rng.uniform(11)) / 20.0;  // 0, 0.05, ..., 0.5
  c.val_fraction = static_cast<double>(rng.uniform(11)) / 20.0;
  c.test_min = static_cast<std::uint32_t>(rng.uniform(3));
  c.test_max = c.test_min + static_cast<std::uint32_t>(rng.uniform(10));
  c.val_min = static_cast<std::uint32_t>(rng.uniform(3));
  c.val_max = c.val_min + static_cast<std::uint32_t>(rng.uniform(10));
  return c;
}

Dataset one_user(const std::vector<std::pair<ItemIdx, std::int64_t>>& item_ts, std::uint32_t n_items) {
  std::vector<Triplet> t;
  for (auto [item, ts] : item_ts) t.push_back({0, item, 4.0f, ts});
  return recs::make_dataset("one", 1, n_items, std::move(t));
}

std::set<ItemIdx> items_of(const std::vector<Rating>& xs) {
  std::set<ItemIdx> s;
  for (const auto& r : xs) s.insert(r.item);
  return s;
}

}  // namespace

TEST_CASE("split invariants hold on random datasets (property, 300 seeds)", "[split][property]") {
  int infeasible_cases = 0;
  recs::testing::for_seeds(300, 20261110, [&](recs::Rng& rng, std::uint64_t) {
    const Dataset full = random_dataset(rng);
    const SplitConfig c = random_config(rng);
    INFO("config " << config_json(c));
    bool feasible = true;
    for (UserIdx u = 0; u < full.n_users(); ++u) {
      const auto n = full.by_user.row_size(u);
      if (n >= c.min_ratings && !expected_sizes(n, c).feasible) feasible = false;
    }
    if (!feasible) {
      // The clamp rule asks for more held-out ratings than the user has: refuse, never guess.
      ++infeasible_cases;
      CHECK_THROWS_AS(recs::make_split(full, c), std::invalid_argument);
      return;
    }
    const Split sp = recs::make_split(full, c);
    check_split(full, c, sp);
  });
  // Most cases must exercise the real path.
  CHECK(infeasible_cases < 60);
}

TEST_CASE("split hand fixture: chronological tail, under-min user, ties by seed", "[split]") {
  // user 0: items 0..9, ts(item i) = 1000 + 10 * ((7 * i) % 10). (7i mod 10) for i = 0..9 is
  //   0, 7, 4, 1, 8, 5, 2, 9, 6, 3, so the chronological order is items
  //   0, 3, 6, 9, 2, 5, 8, 1, 4, 7.
  //   n = 10: n_test = clamp(round(2.0), 1, 10) = 2 -> test = {4, 7}.
  //   n_train = 8: n_val = clamp(round(1.6), 1, 10) = 2 -> validation = {8, 1}.
  //   train_minus_val = {0, 3, 6, 9, 2, 5}.
  // user 1: 9 ratings (< min_ratings 10): all train, not evaluated.
  // user 2: 13 ratings on items 0..12, all at the same ts. n_test = round(2.6) = 3,
  //   n_train = 10, n_val = round(2.0) = 2. The order is decided by mix_seed(42, 2, item).
  std::vector<Triplet> t;
  for (ItemIdx i = 0; i < 10; ++i) t.push_back({0, i, 4.0f, 1000 + 10 * ((7 * i) % 10)});
  for (ItemIdx i = 0; i < 9; ++i) t.push_back({1, i, 3.0f, 5});
  for (ItemIdx i = 0; i < 13; ++i) t.push_back({2, i, 5.0f, 777});
  const Dataset full = recs::make_dataset("hand", 3, 13, t);
  const SplitConfig c;  // defaults: min 10, 0.2 / 0.2, [1, 10], seed 42
  const Split sp = recs::make_split(full, c);
  check_split(full, c, sp);

  CHECK(items_of(sp.test[0]) == std::set<ItemIdx>{4, 7});
  CHECK(items_of(sp.validation[0]) == std::set<ItemIdx>{8, 1});
  CHECK(sp.train_minus_val.by_user.row_size(0) == 6);
  CHECK(sp.train.by_user.row_size(0) == 8);

  CHECK(sp.train.by_user.row_size(1) == 9);
  CHECK(sp.test[1].empty());
  CHECK(sp.eval_users == std::vector<UserIdx>{0, 2});

  std::vector<ItemIdx> order(13);
  for (ItemIdx i = 0; i < 13; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [](ItemIdx a, ItemIdx b) { return recs::mix_seed(42, 2, a) < recs::mix_seed(42, 2, b); });
  CHECK(items_of(sp.test[2]) == std::set<ItemIdx>{order[10], order[11], order[12]});
  CHECK(items_of(sp.validation[2]) == std::set<ItemIdx>{order[8], order[9]});

  // Changing only the seed reorders the tied user (with overwhelming probability for 13 items).
  SplitConfig c2 = c;
  c2.seed = 43;
  const Split sp2 = recs::make_split(full, c2);
  check_split(full, c2, sp2);
  CHECK(items_of(sp2.test[0]) == std::set<ItemIdx>{4, 7});  // distinct timestamps: unaffected
  CHECK(items_of(sp2.test[2]) != items_of(sp.test[2]));
}

TEST_CASE("split sizes follow the clamp / round-half-up rule", "[split]") {
  auto sizes = [](std::uint32_t n, const SplitConfig& c) {
    std::vector<std::pair<ItemIdx, std::int64_t>> xs;
    for (ItemIdx i = 0; i < n; ++i) xs.push_back({i, static_cast<std::int64_t>(i)});
    const Split sp = recs::make_split(one_user(xs, n), c);
    return std::pair<std::size_t, std::size_t>{sp.test[0].size(), sp.validation[0].size()};
  };
  using P = std::pair<std::size_t, std::size_t>;
  const SplitConfig d;  // test 0.2 [1, 10], val 0.2 [1, 10]
  CHECK(sizes(10, d) == P{2, 2});    // 2.0 -> 2; train 8: 1.6 -> 2
  CHECK(sizes(12, d) == P{2, 2});    // 2.4 -> 2; train 10: 2.0 -> 2
  CHECK(sizes(13, d) == P{3, 2});    // 2.6 -> 3; train 10: 2.0 -> 2
  CHECK(sizes(17, d) == P{3, 3});    // 3.4 -> 3; train 14: 2.8 -> 3
  CHECK(sizes(30, d) == P{6, 5});    // 6.0 -> 6; train 24: 4.8 -> 5
  CHECK(sizes(60, d) == P{10, 10});  // 12 -> clamp 10; train 50: 10 -> 10
  CHECK(sizes(100, d) == P{10, 10}); // 20 -> clamp 10; train 90: 18 -> clamp 10

  SplitConfig h;  // exact binary halves to check round-half-up
  h.min_ratings = 1;
  h.test_fraction = 0.25;
  h.val_fraction = 0.5;
  h.test_max = 100;
  h.val_max = 100;
  CHECK(sizes(10, h) == P{3, 4});  // 2.5 -> 3 (half up); train 7: 3.5 -> 4
  CHECK(sizes(2, h) == P{1, 1});   // 0.5 -> 1; train 1: 0.5 -> 1 (train_minus_val empty)
  CHECK(sizes(6, h) == P{2, 2});   // 1.5 -> 2; train 4: 2.0 -> 2

  SplitConfig z;  // minimums of zero allow empty held-out sets
  z.min_ratings = 1;
  z.test_fraction = 0.0;
  z.val_fraction = 0.0;
  z.test_min = 0;
  z.val_min = 0;
  CHECK(sizes(5, z) == P{0, 0});

  // n = 1 with the half-up config: n_test = clamp(0.25 -> 0, 1, 100) = 1, n_train = 0,
  // n_val = clamp(0, 1, 100) = 1 > 0 ratings left. The rule cannot be satisfied: refuse.
  CHECK_THROWS_AS(sizes(1, h), std::invalid_argument);
}

TEST_CASE("split cold_item_share hand fixture", "[split]") {
  // user 0 rates items 0..9 at ts = item, and item 10 at ts 100 (n = 11).
  //   n_test = round(2.2) = 2 -> test = {9, 10}. user 1 (1 rating on item 9, not evaluated)
  //   keeps item 9 in train. Item 10 has no train rating: cold share = 1 / 2 = 0.5.
  std::vector<Triplet> t;
  for (ItemIdx i = 0; i < 10; ++i) t.push_back({0, i, 4.0f, static_cast<std::int64_t>(i)});
  t.push_back({0, 10, 4.0f, 100});
  t.push_back({1, 9, 4.0f, 0});
  const Dataset full = recs::make_dataset("cold", 2, 11, t);
  const Split sp = recs::make_split(full, SplitConfig{});
  check_split(full, SplitConfig{}, sp);
  CHECK(items_of(sp.test[0]) == std::set<ItemIdx>{9, 10});
  CHECK(sp.cold_item_share == 0.5);

  // No evaluated users: no test ratings, share 0.
  const Dataset small = recs::make_dataset("small", 1, 2, {{0, 0, 4.0f, 0}});
  CHECK(recs::make_split(small, SplitConfig{}).cold_item_share == 0.0);
}

TEST_CASE("split drops tags on held-out pairs (leakage guard)", "[split][tags]") {
  // user 0 rates items 0..9 at ts = item: test {8, 9}, validation {6, 7}, train-minus-val {0..5}.
  // user 1 rates only item 9 (not evaluated, all train).
  // Tags:
  //   A (user 0, item 9): test pair        -> dropped from train and train_minus_val
  //   B (user 0, item 7): validation pair  -> kept in train, dropped from train_minus_val
  //   C (user 0, item 2): train pair       -> kept in both
  //   D (user 0, item 10): item not rated by user 0 -> kept in both
  //   E (user 1, item 9): user 1 not evaluated -> kept in both
  //   F (external user 50, item 9): no ratings at all -> kept in both
  std::vector<Triplet> t;
  for (ItemIdx i = 0; i < 10; ++i) t.push_back({0, i, 4.0f, static_cast<std::int64_t>(i)});
  t.push_back({1, 9, 4.0f, 0});
  Dataset full = recs::make_dataset("tags", 2, 11, t);
  const recs::TagApplication A{0, 9, "A", 1}, B{0, 7, "B", 2}, C{0, 2, "C", 3}, D{0, 10, "D", 4},
      E{1, 9, "E", 5}, F{50, 9, "F", 6};
  full.tags = {A, B, C, D, E, F};
  const Split sp = recs::make_split(full, SplitConfig{});
  check_split(full, SplitConfig{}, sp);
  REQUIRE(items_of(sp.test[0]) == std::set<ItemIdx>{8, 9});
  REQUIRE(items_of(sp.validation[0]) == std::set<ItemIdx>{6, 7});
  CHECK(sp.train.tags == std::vector<recs::TagApplication>{B, C, D, E, F});
  CHECK(sp.train_minus_val.tags == std::vector<recs::TagApplication>{C, D, E, F});

  // Tags never enter the hash.
  Dataset no_tags = full;
  no_tags.tags.clear();
  CHECK(recs::make_split(no_tags, SplitConfig{}).hash == sp.hash);
}

TEST_CASE("split hash: exact encoding, stability, sensitivity", "[split][hash]") {
  SECTION("literal encoding for a one-rating dataset") {
    // One user with one rating (< 10): set t. Config JSON as nlohmann::json::dump() writes it.
    const Dataset full = recs::make_dataset("h", 1, 1, {{0, 0, 4.0f, 0}});
    const Split sp = recs::make_split(full, SplitConfig{});
    const std::string expected_input =
        R"({"min_ratings":10,"seed":42,"test_fraction":0.2,"test_max":10,"test_min":1,)"
        R"("val_fraction":0.2,"val_max":10,"val_min":1})"
        "0,0,t\n";
    CHECK(sp.hash == recs::sha256_hex(expected_input));
    CHECK(sp.hash.size() == 64);
  }
  SECTION("literal encoding with all three sets") {
    // user 0 rates items 0..9 at ts = item: test {8, 9}, val {6, 7}, the rest t.
    std::vector<std::pair<ItemIdx, std::int64_t>> xs;
    for (ItemIdx i = 0; i < 10; ++i) xs.push_back({i, i});
    const Split sp = recs::make_split(one_user(xs, 10), SplitConfig{});
    const std::string expected_input =
        R"({"min_ratings":10,"seed":42,"test_fraction":0.2,"test_max":10,"test_min":1,)"
        R"("val_fraction":0.2,"val_max":10,"val_min":1})"
        "0,0,t\n0,1,t\n0,2,t\n0,3,t\n0,4,t\n0,5,t\n0,6,v\n0,7,v\n0,8,x\n0,9,x\n";
    CHECK(sp.hash == recs::sha256_hex(expected_input));
  }

  recs::Rng rng(99);
  Dataset full = random_dataset(rng);
  while (full.n_ratings() < 50) full = random_dataset(rng);
  SplitConfig c;
  c.min_ratings = 3;
  const Split base = recs::make_split(full, c);

  SECTION("identical inputs and config give identical splits") {
    const Split again = recs::make_split(full, c);
    CHECK(same_split(base, again));
  }
  SECTION("a different seed changes the hash") {
    SplitConfig c2 = c;
    c2.seed = c.seed + 1;
    CHECK(recs::make_split(full, c2).hash != base.hash);
  }
  SECTION("any config field changes the hash") {
    SplitConfig c2 = c;
    c2.val_max = c.val_max + 1;
    CHECK(recs::make_split(full, c2).hash != base.hash);
  }
  SECTION("adding or removing a rating changes the hash") {
    std::vector<Triplet> t;
    for (UserIdx u = 0; u < full.n_users(); ++u) {
      const auto r = row_ratings(full.by_user, u);
      for (const auto& x : r) t.push_back({u, x.item, x.value, x.ts});
    }
    std::vector<Triplet> fewer(t.begin() + 1, t.end());
    const Dataset d_fewer = recs::make_dataset("x", full.n_users(), full.n_items(), fewer);
    CHECK(recs::make_split(d_fewer, c).hash != base.hash);

    std::vector<Triplet> more = t;
    more.push_back({0, full.n_items(), 4.0f, 0});  // a new item, so no duplicate
    const Dataset d_more = recs::make_dataset("x", full.n_users(), full.n_items() + 1, more);
    CHECK(recs::make_split(d_more, c).hash != base.hash);
  }
  SECTION("a timestamp change that moves a rating between sets changes the hash") {
    // user 0 rates items 0..9 at ts = item: test {8, 9}. Moving item 0 to ts 100 puts it in test.
    std::vector<std::pair<ItemIdx, std::int64_t>> xs;
    for (ItemIdx i = 0; i < 10; ++i) xs.push_back({i, i});
    const Split a = recs::make_split(one_user(xs, 10), SplitConfig{});
    xs[0].second = 100;
    const Split b = recs::make_split(one_user(xs, 10), SplitConfig{});
    CHECK(items_of(b.test[0]) == std::set<ItemIdx>{0, 9});
    CHECK(a.hash != b.hash);
  }
  SECTION("a rating value change alone does not change the hash (header: it identifies the assignment)") {
    // split.hpp: "Ratings' values/timestamps are covered by the dataset itself; the hash identifies
    // the assignment." A value change leaves every (user, item, set) unchanged.
    std::vector<std::pair<ItemIdx, std::int64_t>> xs;
    for (ItemIdx i = 0; i < 10; ++i) xs.push_back({i, i});
    const Dataset a = one_user(xs, 10);
    std::vector<Triplet> t;
    for (auto [item, ts] : xs) t.push_back({0, item, item == 3 ? 1.0f : 4.0f, ts});
    const Dataset b = recs::make_dataset("one", 1, 10, t);
    CHECK(recs::make_split(a, SplitConfig{}).hash == recs::make_split(b, SplitConfig{}).hash);
  }
}

TEST_CASE("split output does not depend on the thread count", "[split]") {
  // 3000 users, up to 60 ratings each, timestamps in a small range (many ties).
  recs::Rng rng(2026);
  const std::uint32_t n_users = 3000, n_items = 500;
  std::vector<Triplet> t;
  std::vector<ItemIdx> items(n_items);
  for (ItemIdx i = 0; i < n_items; ++i) items[i] = i;
  for (UserIdx u = 0; u < n_users; ++u) {
    rng.shuffle(items.begin(), items.end());
    const auto count = rng.uniform(61);
    for (std::uint64_t j = 0; j < count; ++j)
      t.push_back({u, items[j], 0.5f * static_cast<float>(1 + rng.uniform(10)),
                   static_cast<std::int64_t>(rng.uniform(20))});
  }
  const Dataset full = recs::make_dataset("threads", n_users, n_items, std::move(t));
  const int saved = omp_get_max_threads();
  omp_set_num_threads(1);
  const Split one = recs::make_split(full, SplitConfig{});
  omp_set_num_threads(4);
  const Split four = recs::make_split(full, SplitConfig{});
  omp_set_num_threads(saved);
  CHECK(same_split(one, four));
  check_split(full, SplitConfig{}, four);
}

TEST_CASE("split rejects invalid configs", "[split]") {
  const Dataset full = recs::make_dataset("x", 1, 1, {{0, 0, 4.0f, 0}});
  auto bad = [&](auto mutate) {
    SplitConfig c;
    mutate(c);
    CHECK_THROWS_AS(recs::make_split(full, c), std::invalid_argument);
  };
  bad([](SplitConfig& c) { c.test_fraction = -0.1; });
  bad([](SplitConfig& c) { c.test_fraction = 1.5; });
  bad([](SplitConfig& c) { c.val_fraction = std::nan(""); });
  bad([](SplitConfig& c) { c.test_min = 5; c.test_max = 4; });
  bad([](SplitConfig& c) { c.val_min = 3; c.val_max = 2; });
}
