// Temporal train / validation / test split (spec: Evaluation protocol -> Split; the rule is frozen
// in recs/split.hpp).
//
// Why temporal: a recommender is used to predict what a user will rate NEXT. Holding out each
// user's most recent ratings (a "leave-last-out" style split, see e.g. Campos, Diez & Cantador,
// "Time-aware recommender systems: a comprehensive survey and analysis of existing evaluation
// protocols", UMUAI 2014) evaluates exactly that, whereas a random split lets the model peek at
// the future.
//
// Per user u with n >= min_ratings ratings:
//   1. sort the ratings by (ts, mix_seed(seed, u, item)); the seeded key orders equal timestamps
//      deterministically without favouring low item indices (the item breaks a 64-bit key
//      collision, which in practice never happens)
//   2. n_test  = clamp(round_half_up(test_fraction * n), test_min, test_max): the last n_test
//   3. n_train = n - n_test; n_val = clamp(round_half_up(val_fraction * n_train), val_min, val_max):
//      the last n_val of the remaining ratings
//   round_half_up(x) = floor(x + 0.5), evaluated in double.
// If the clamped sizes do not fit (n_test > n or n_val > n_train, only possible with a tiny
// min_ratings) the rule has no answer, so make_split throws std::invalid_argument instead of
// silently shrinking a set.
// Users below min_ratings keep everything in train and train_minus_val.
//
// Outputs: `train` = train-minus-val + validation, `train_minus_val`, and per-user validation and
// test vectors in ascending item order. Both Datasets keep the input's user/item index spaces,
// IdMaps and metadata (movies, genome) unchanged; only the rating matrices and tags differ.
//
// Tags (leakage guard): a user's tag on a held-out movie reveals the interaction, so
//   `train`           drops every tag application whose (tagging user, item) is in that user's test set;
//   `train_minus_val` also drops those whose (tagging user, item) is in the user's validation set.
// The tag's external user_id is mapped through full.users; tags by users with no ratings, and tags
// on items the tagger did not rate, are kept. Tags are not part of the hash.
//
// Hash (reproducible from this description alone): SHA-256 over
//   the config as nlohmann::json::dump() of {"min_ratings","seed","test_fraction","test_max",
//   "test_min","val_fraction","val_max","val_min"} (std::map, so keys are sorted, no spaces),
//   followed by, for each user in index order and each of that user's ratings in item order,
//   the ASCII bytes "<user>,<item>,<set>\n" with set = t (train-minus-val), v (validation),
//   x (test). Decimal indices, no padding.
//
// cold_item_share = #test ratings whose item has no rating in `train` / #test ratings (0 if there
// are no test ratings).
//
// Parallelism: the per-user sort runs under OpenMP with a static schedule; each user writes only
// its own slots, and everything order-dependent (CSR assembly, hash) runs serially, so the
// output does not depend on the thread count.
#include "recs/split.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "recs/rng.hpp"
#include "recs/sha256.hpp"
#include "recs/sparse.hpp"

namespace recs {

namespace {

enum Set : char { kTrainMinusVal = 't', kValidation = 'v', kTest = 'x' };

std::uint64_t round_half_up(double x) { return static_cast<std::uint64_t>(std::floor(x + 0.5)); }

std::uint64_t clamp_u64(std::uint64_t x, std::uint64_t lo, std::uint64_t hi) {
  return std::min(std::max(x, lo), hi);
}

void validate_config(const SplitConfig& c) {
  auto fraction_ok = [](double f) { return std::isfinite(f) && f >= 0.0 && f <= 1.0; };
  if (!fraction_ok(c.test_fraction)) throw std::invalid_argument("make_split: test_fraction must be in [0, 1]");
  if (!fraction_ok(c.val_fraction)) throw std::invalid_argument("make_split: val_fraction must be in [0, 1]");
  if (c.test_min > c.test_max) throw std::invalid_argument("make_split: test_min > test_max");
  if (c.val_min > c.val_max) throw std::invalid_argument("make_split: val_min > val_max");
}

std::string config_json(const SplitConfig& c) {
  nlohmann::json j;  // object keys are kept sorted by nlohmann::json's default std::map
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

// A Dataset with `full`'s index spaces and metadata but only the ratings whose assignment
// passes `keep`.
template <class Keep>
Dataset filtered_copy(const Dataset& full, const std::vector<char>& assign, Keep keep) {
  Dataset d;
  d.name = full.name;
  d.users = full.users;
  d.items = full.items;
  d.movies = full.movies;
  d.genome = full.genome;

  const Csr& src = full.by_user;
  // Leakage guard: a tag stays unless its tagger is a rating user whose rating of the same item
  // was moved out of this Dataset (to validation or test).
  for (const TagApplication& t : full.tags) {
    const auto u = full.users.find(t.user_id);
    if (u && *u < src.n_rows) {
      const auto row = src.row_indices(*u);
      const auto it = std::lower_bound(row.begin(), row.end(), t.item);
      if (it != row.end() && *it == t.item) {
        const std::uint64_t p = src.indptr[*u] + static_cast<std::uint64_t>(it - row.begin());
        if (!keep(assign[p])) continue;
      }
    }
    d.tags.push_back(t);
  }

  Csr& m = d.by_user;
  m.n_rows = src.n_rows;
  m.n_cols = src.n_cols;
  m.indptr.assign(1, 0);
  m.indptr.reserve(static_cast<std::size_t>(src.n_rows) + 1);
  for (std::uint32_t u = 0; u < src.n_rows; ++u) {
    for (std::uint64_t p = src.indptr[u]; p < src.indptr[u + 1]; ++p) {
      if (!keep(assign[p])) continue;
      m.indices.push_back(src.indices[p]);
      m.values.push_back(src.values[p]);
      m.ts.push_back(src.ts[p]);
    }
    m.indptr.push_back(m.indices.size());
  }
  d.by_item = transpose(m);
  return d;
}

}  // namespace

Split make_split(const Dataset& full, const SplitConfig& cfg) {
  validate_config(cfg);
  const Csr& R = full.by_user;
  const std::uint32_t n_users = R.n_rows;

  // assign[p] is the set of the rating stored at CSR position p (row order = item order).
  std::vector<char> assign(R.nnz(), kTrainMinusVal);
  std::vector<char> infeasible(n_users, 0);

  const auto n_users_signed = static_cast<std::int64_t>(n_users);
#pragma omp parallel for schedule(static)
  for (std::int64_t su = 0; su < n_users_signed; ++su) {
    const auto u = static_cast<std::uint32_t>(su);
    const std::uint64_t n = R.row_size(u);
    if (n < cfg.min_ratings) continue;

    const std::uint64_t n_test =
        clamp_u64(round_half_up(cfg.test_fraction * static_cast<double>(n)), cfg.test_min, cfg.test_max);
    if (n_test > n) {
      infeasible[u] = 1;
      continue;
    }
    const std::uint64_t n_train = n - n_test;
    const std::uint64_t n_val =
        clamp_u64(round_half_up(cfg.val_fraction * static_cast<double>(n_train)), cfg.val_min, cfg.val_max);
    if (n_val > n_train) {
      infeasible[u] = 1;
      continue;
    }

    // Chronological order of the user's ratings: positions 0..n-1 within the row.
    const auto items = R.row_indices(u);
    const auto ts = R.row_ts(u);
    struct Key {
      std::int64_t ts;
      std::uint64_t tie;
      ItemIdx item;
      std::uint64_t pos;
    };
    std::vector<Key> order(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      order[i] = {ts[i], mix_seed(cfg.seed, u, items[i]), items[i], i};
    }
    std::sort(order.begin(), order.end(), [](const Key& a, const Key& b) {
      if (a.ts != b.ts) return a.ts < b.ts;
      if (a.tie != b.tie) return a.tie < b.tie;
      return a.item < b.item;
    });
    const std::uint64_t base = R.indptr[u];
    for (std::uint64_t r = n_train; r < n; ++r) assign[base + order[r].pos] = kTest;
    for (std::uint64_t r = n_train - n_val; r < n_train; ++r) assign[base + order[r].pos] = kValidation;
  }

  for (std::uint32_t u = 0; u < n_users; ++u) {
    if (infeasible[u]) {
      throw std::invalid_argument(
          "make_split: user " + std::to_string(u) + " with " + std::to_string(R.row_size(u)) +
          " ratings cannot hold the clamped test/validation sizes; raise min_ratings or lower "
          "test_min/val_min");
    }
  }

  Split s;
  s.config = cfg;
  s.train = filtered_copy(full, assign, [](char a) { return a != kTest; });
  s.train_minus_val = filtered_copy(full, assign, [](char a) { return a == kTrainMinusVal; });
  s.validation.assign(n_users, {});
  s.test.assign(n_users, {});

  Sha256 hasher;
  hasher.update(config_json(cfg));
  std::string line;
  for (std::uint32_t u = 0; u < n_users; ++u) {
    const std::uint64_t n = R.row_size(u);
    if (n >= cfg.min_ratings) s.eval_users.push_back(u);
    const std::string prefix = std::to_string(u) + ",";
    for (std::uint64_t p = R.indptr[u]; p < R.indptr[u + 1]; ++p) {
      const Rating r{R.indices[p], R.values[p], R.ts[p]};
      if (assign[p] == kTest) s.test[u].push_back(r);
      if (assign[p] == kValidation) s.validation[u].push_back(r);
      line = prefix;
      line += std::to_string(r.item);
      line += ',';
      line += assign[p];
      line += '\n';
      hasher.update(line);
    }
  }
  s.hash = hasher.hex_digest();

  std::uint64_t n_test_total = 0, n_cold = 0;
  for (std::uint32_t u = 0; u < n_users; ++u) {
    for (const Rating& r : s.test[u]) {
      ++n_test_total;
      if (s.train.by_item.row_size(r.item) == 0) ++n_cold;
    }
  }
  s.cold_item_share =
      n_test_total == 0 ? 0.0 : static_cast<double>(n_cold) / static_cast<double>(n_test_total);
  return s;
}

}  // namespace recs
