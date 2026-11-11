// Phase 3 gates and contract tests for the MF-SGD model (spec: Models -> Matrix factorization;
// Phase 3 gates). The ml-32m gates (RMSE vs bias baseline, 500-user parity) run after Wave 4.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <vector>

#include <omp.h>

#include <catch2/catch_test_macros.hpp>

#include "../../data/temp_dir.hpp"
#include "../../../../core/src/models/mf_sgd_detail.hpp"
#include "property.hpp"
#include "recs/dataset.hpp"
#include "recs/metrics.hpp"
#include "recs/model.hpp"
#include "recs/synthetic.hpp"

namespace {

using recs::mf_detail::Params;

// --- helpers -------------------------------------------------------------------------------

Params random_params(recs::Rng& rng, std::uint32_t nu, std::uint32_t ni, std::uint32_t k) {
  Params m;
  m.k = k;
  m.mu = 3.5;
  m.bu.resize(nu);
  m.bi.resize(ni);
  m.P.resize(std::size_t{nu} * k);
  m.Q.resize(std::size_t{ni} * k);
  for (auto& x : m.bu) x = 0.5 * rng.normal();
  for (auto& x : m.bi) x = 0.5 * rng.normal();
  for (auto& x : m.P) x = 0.5 * rng.normal();
  for (auto& x : m.Q) x = 0.5 * rng.normal();
  return m;
}

std::vector<recs::Triplet> random_ratings(recs::Rng& rng, std::uint32_t nu, std::uint32_t ni,
                                          double density) {
  std::vector<recs::Triplet> out;
  for (std::uint32_t u = 0; u < nu; ++u)
    for (std::uint32_t i = 0; i < ni; ++i)
      if (rng.uniform01() < density)
        out.push_back({u, i, static_cast<float>(0.5 * (1 + rng.uniform(10))), 0});
  return out;
}

// The four parameter blocks, addressed uniformly.
std::vector<double>& block(Params& m, int b) {
  switch (b) {
    case 0: return m.bu;
    case 1: return m.bi;
    case 2: return m.P;
    default: return m.Q;
  }
}
const char* block_name(int b) {
  static const char* names[] = {"b_u", "b_i", "P", "Q"};
  return names[b];
}

double rel_error(const std::vector<double>& a, const std::vector<double>& b) {
  double diff = 0.0, na = 0.0, nb = 0.0;
  for (std::size_t j = 0; j < a.size(); ++j) {
    diff += (a[j] - b[j]) * (a[j] - b[j]);
    na += a[j] * a[j];
    nb += b[j] * b[j];
  }
  const double denom = std::max(std::sqrt(na), std::sqrt(nb));
  return denom == 0.0 ? 0.0 : std::sqrt(diff) / denom;
}

// Central finite-difference gradient of objective() for one parameter block.
std::vector<double> fd_gradient(const Params& m0, int b, std::span<const recs::Triplet> ratings,
                                double lambda, double h) {
  Params m = m0;
  auto& theta = block(m, b);
  std::vector<double> g(theta.size());
  for (std::size_t j = 0; j < theta.size(); ++j) {
    const double saved = theta[j];
    theta[j] = saved + h;
    const double fp = recs::mf_detail::objective(m, ratings, lambda);
    theta[j] = saved - h;
    const double fm = recs::mf_detail::objective(m, ratings, lambda);
    theta[j] = saved;
    g[j] = (fp - fm) / (2.0 * h);
  }
  return g;
}

// Top-k by score descending, ItemIdx ascending, excluding the user's rated items (independent of
// the shared recommend(), which belongs to another workstream).
std::vector<recs::ItemIdx> top_k(std::span<const float> scores, std::span<const recs::Rating> rated,
                                 std::size_t k) {
  std::vector<bool> excluded(scores.size(), false);
  for (const auto& r : rated) excluded[r.item] = true;
  std::vector<recs::ItemIdx> items;
  for (recs::ItemIdx i = 0; i < scores.size(); ++i)
    if (!excluded[i] && std::isfinite(scores[i])) items.push_back(i);
  std::sort(items.begin(), items.end(), [&](recs::ItemIdx a, recs::ItemIdx b) {
    if (scores[a] != scores[b]) return scores[a] > scores[b];
    return a < b;
  });
  if (items.size() > k) items.resize(k);
  return items;
}

std::vector<recs::Rating> user_ratings(const recs::Dataset& ds, recs::UserIdx u) {
  std::vector<recs::Rating> out;
  const auto items = ds.by_user.row_indices(u);
  const auto vals = ds.by_user.row_values(u);
  const auto ts = ds.by_user.row_ts(u);
  for (std::size_t j = 0; j < items.size(); ++j) out.push_back({items[j], vals[j], ts[j]});
  return out;
}

// Moves a seeded `fraction` of each user's training ratings into a validation set, so that early
// stopping never looks at the held-out test ratings.
struct TrainValidation {
  recs::Dataset train;
  std::vector<std::vector<recs::Rating>> validation;
};
TrainValidation carve_validation(const recs::Dataset& ds, double fraction, std::uint64_t seed) {
  TrainValidation out;
  out.validation.resize(ds.n_users());
  std::vector<recs::Triplet> keep;
  for (recs::UserIdx u = 0; u < ds.n_users(); ++u) {
    auto rs = user_ratings(ds, u);
    recs::Rng rng(recs::mix_seed(seed, u));
    rng.shuffle(rs.begin(), rs.end());
    const auto n_val = static_cast<std::size_t>(fraction * static_cast<double>(rs.size()));
    for (std::size_t j = 0; j < rs.size(); ++j) {
      if (j < n_val && rs.size() - n_val >= 1)
        out.validation[u].push_back(rs[j]);
      else
        keep.push_back({u, rs[j].item, rs[j].value, rs[j].ts});
    }
  }
  out.train = recs::make_dataset("train", ds.n_users(), ds.n_items(), std::move(keep));
  return out;
}

double heldout_rmse(const recs::Model& m, const recs::Dataset& train,
                    const std::vector<std::vector<recs::Rating>>& heldout, bool trained_path) {
  std::vector<float> scores(m.n_items());
  std::vector<float> predicted;
  std::vector<recs::Rating> actual;
  for (recs::UserIdx u = 0; u < train.n_users(); ++u) {
    if (heldout[u].empty()) continue;
    if (trained_path) {
      m.score_trained_user(u, scores);
    } else {
      const auto rs = user_ratings(train, u);
      m.score(m.fold_in(rs), scores);
    }
    for (const auto& r : heldout[u]) {
      predicted.push_back(scores[r.item]);
      actual.push_back(r);
    }
  }
  return recs::rating_errors(predicted, actual).rmse;
}

double mean_parity_jaccard(const recs::Model& m, const recs::Dataset& train) {
  std::vector<float> batch(m.n_items()), folded(m.n_items());
  double sum = 0.0;
  std::size_t n = 0;
  for (recs::UserIdx u = 0; u < train.n_users(); ++u) {
    const auto rs = user_ratings(train, u);
    if (rs.empty()) continue;
    m.score_trained_user(u, batch);
    m.score(m.fold_in(rs), folded);
    sum += recs::jaccard_at_k(top_k(batch, rs, 10), top_k(folded, rs, 10), 10);
    ++n;
  }
  return sum / static_cast<double>(n);
}

// low_rank_explicit has no biases (mu = b_u = b_i = 0), so on its own it cannot catch bias bugs.
// This adds a known structure on top: r'_ui = 3.5 + b_u + b_i + r_ui with b_u, b_i ~ N(0, 0.5^2)
// drawn from a seeded stream, applied to training and held-out ratings alike. Optionally rounds
// to 0.5 stars clamped to [0.5, 5] (adds rounding noise, like real ratings).
struct Biased {
  recs::Dataset train;
  std::vector<std::vector<recs::Rating>> heldout;
};
Biased add_biases(const recs::synthetic::ExplicitLowRank& s, std::uint64_t seed, bool stars) {
  recs::Rng rng(recs::mix_seed(seed, 77));
  std::vector<double> bu(s.train.n_users()), bi(s.train.n_items());
  for (auto& b : bu) b = 0.5 * rng.normal();
  for (auto& b : bi) b = 0.5 * rng.normal();
  const auto shift = [&](recs::UserIdx u, recs::ItemIdx i, float v) {
    double x = 3.5 + bu[u] + bi[i] + v;
    if (stars) x = std::clamp(std::round(x * 2.0) / 2.0, 0.5, 5.0);
    return static_cast<float>(x);
  };
  Biased out;
  std::vector<recs::Triplet> t;
  for (recs::UserIdx u = 0; u < s.train.n_users(); ++u)
    for (const auto& r : user_ratings(s.train, u)) t.push_back({u, r.item, shift(u, r.item, r.value), r.ts});
  out.train = recs::make_dataset("biased", s.train.n_users(), s.train.n_items(), std::move(t));
  out.heldout = s.heldout;
  for (recs::UserIdx u = 0; u < out.heldout.size(); ++u)
    for (auto& r : out.heldout[u]) r.value = shift(u, r.item, r.value);
  return out;
}

// The configuration for the synthetic gates.
//  - k = 5: the true rank (the gate is about recovering a known structure, not choosing k).
//  - lambda = 0.005: chosen on an independent draw (same sizes, seed 32, never the gate's seed 31)
//    by best-epoch RMSE on that draw's held-out set (used as the validation set), other settings
//    as below. Re-run on 2026-11-10 against this code (release build):
//      lambda 0.002 / 0.005 / 0.01 / 0.02 / 0.05 -> 0.1129 / 0.1136 / 0.1167 / 0.1283 / 0.1862.
//    The regulariser is per rating (lambda * n per user), and the factor entries here are small
//    (sd 5^-1/4 ~ 0.67), so the ml-scale default 0.05 over-shrinks them. 0.005 sits in the flat
//    region while keeping some ridge for the fold-in solve.
//  - lr = 0.02 with 0.98 decay, early stopping on a validation set carved from training
//    (patience 5): stops after roughly 100-150 epochs.
recs::Config synthetic_config() {
  return {{"k", 5},            {"lambda", 0.005}, {"lr", 0.02},  {"lr_decay", 0.98},
          {"max_epochs", 300}, {"patience", 5},   {"init_sd", 0.1}};
}

}  // namespace

// --- Phase 3 gate: gradient check ----------------------------------------------------------

TEST_CASE("MF analytic gradient matches central finite differences", "[mf][gate]") {
  recs::testing::for_seeds(10, 2026, [](recs::Rng& rng, std::uint64_t) {
    const std::uint32_t nu = 6, ni = 7, k = 4;
    const Params m = random_params(rng, nu, ni, k);
    const auto ratings = random_ratings(rng, nu, ni, 0.5);
    const double lambda = 0.1;
    Params g = recs::mf_detail::gradient(m, ratings, lambda);
    for (int b = 0; b < 4; ++b) {
      INFO("block " << block_name(b));
      const auto fd = fd_gradient(m, b, ratings, lambda, 1e-5);
      const double err = rel_error(block(g, b), fd);
      INFO("relative error " << err);
      CHECK(err < 1e-5);
    }
  });
}

TEST_CASE("One SGD step equals a finite-difference gradient step on that rating", "[mf][gate]") {
  // Ties the gradient check to the code training runs: sgd_step must move every parameter by
  // exactly -lr * d(per-rating loss)/d(theta).
  recs::testing::for_seeds(10, 77, [](recs::Rng& rng, std::uint64_t) {
    const std::uint32_t nu = 4, ni = 5, k = 3;
    const Params m = random_params(rng, nu, ni, k);
    const recs::Triplet r{static_cast<std::uint32_t>(rng.uniform(nu)),
                          static_cast<std::uint32_t>(rng.uniform(ni)), 4.0f, 0};
    const double lambda = 0.05, lr = 0.01;
    Params stepped = m;
    recs::mf_detail::sgd_step(stepped, r, lr, lambda);
    const std::vector<recs::Triplet> one{r};
    for (int b = 0; b < 4; ++b) {
      INFO("block " << block_name(b));
      const auto fd = fd_gradient(m, b, one, lambda, 1e-5);
      Params mm = m;
      std::vector<double> delta(fd.size()), expected(fd.size());
      for (std::size_t j = 0; j < fd.size(); ++j) {
        delta[j] = block(stepped, b)[j] - block(mm, b)[j];
        expected[j] = -lr * fd[j];
      }
      CHECK(rel_error(delta, expected) < 1e-5);
    }
  });
}

// --- Phase 3 gate: synthetic recovery ------------------------------------------------------

TEST_CASE("MF recovers rank-5 synthetic ratings: held-out RMSE < 0.15", "[mf][gate]") {
  // Rank-5 ground truth, N(0, 0.1^2) noise (spec Phase 3). Signal variance is 1, so predicting the
  // mean scores RMSE about 1.0; the noise floor is 0.1.
  const auto s = recs::synthetic::low_rank_explicit(600, 400, 5, 0.1, 0.15, 0.2, 31);
  const auto tv = carve_validation(s.train, 0.1, 5);
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.validation = &tv.validation;
  ctx.seed = 42;
  model->fit(tv.train, synthetic_config(), ctx);

  const double rmse_trained = heldout_rmse(*model, tv.train, s.heldout, true);
  const double rmse_folded = heldout_rmse(*model, tv.train, s.heldout, false);
  INFO("held-out RMSE trained vectors " << rmse_trained << ", fold-in " << rmse_folded
                                        << ", epochs " << model->training_curve().size());
  CHECK(rmse_trained < 0.15);
  CHECK(rmse_folded < 0.15);
}

TEST_CASE("MF fold-in parity: Jaccard@10 against trained user vectors > 0.9", "[mf][gate]") {
  const auto s = recs::synthetic::low_rank_explicit(600, 400, 5, 0.1, 0.15, 0.2, 31);
  const auto tv = carve_validation(s.train, 0.1, 5);
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.validation = &tv.validation;
  model->fit(tv.train, synthetic_config(), ctx);
  REQUIRE(model->supports_trained_scoring());
  const double j = mean_parity_jaccard(*model, tv.train);
  INFO("mean Jaccard@10 " << j);
  CHECK(j > 0.9);
}

// --- Bias gates: synthetic data with known user and item biases ----------------------------

TEST_CASE("MF recovers biased rank-5 synthetic ratings: RMSE < 0.15 and parity > 0.9",
          "[mf][gate]") {
  // Same rank-5 + N(0, 0.1^2) structure as the plain gate, plus mu = 3.5 and b_u, b_i with sd 0.5.
  // Catches a score() that drops b_i (trained-path RMSE ~0.5) and a fold-in that ignores b_i in
  // the residuals (fold-in RMSE far above the noise).
  const auto s = recs::synthetic::low_rank_explicit(600, 400, 5, 0.1, 0.15, 0.2, 31);
  const auto b = add_biases(s, 31, false);
  const auto tv = carve_validation(b.train, 0.1, 5);
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.validation = &tv.validation;
  model->fit(tv.train, synthetic_config(), ctx);
  const double rmse_trained = heldout_rmse(*model, tv.train, b.heldout, true);
  const double rmse_folded = heldout_rmse(*model, tv.train, b.heldout, false);
  const double j = mean_parity_jaccard(*model, tv.train);
  INFO("held-out RMSE trained " << rmse_trained << ", fold-in " << rmse_folded << ", Jaccard@10 "
                                << j << ", epochs " << model->training_curve().size());
  CHECK(rmse_trained < 0.15);
  CHECK(rmse_folded < 0.15);
  CHECK(j > 0.9);
}

TEST_CASE("MF fold-in reproduces converged trained user vectors on biased star ratings",
          "[mf][gate]") {
  // Once SGD has converged, each training user's (b_u, p_u) is the minimiser of their part of the
  // objective with the item parameters fixed, which is exactly what fold_in() solves, including
  // its ridge lambda * n (n = the user's rating count, because the regulariser is per rating).
  // So on biased star ratings from a rank-8 structure, trained long enough without early stopping,
  // folded scores must match trained scores closely. This checks the fold-in math itself: a ridge
  // of lambda instead of lambda * n, or residuals without b_i, break the match.
  const auto s = recs::synthetic::low_rank_explicit(300, 200, 8, 0.3, 0.25, 0.0, 9);
  const auto ds = add_biases(s, 9, true).train;
  auto model = recs::make_mf_sgd();
  model->fit(ds,
             recs::Config{{"k", 8}, {"lambda", 0.05}, {"lr", 0.02}, {"lr_decay", 0.99},
                          {"max_epochs", 400}, {"init_sd", 0.1}},
             recs::TrainContext{});
  // Relative difference of the personalised scores: each user's scores centred on their own mean
  // (removes mu + b_u) for the norm; the difference keeps everything.
  std::vector<float> batch(ds.n_items()), folded(ds.n_items());
  double diff = 0.0, norm = 0.0;
  for (recs::UserIdx u = 0; u < ds.n_users(); ++u) {
    model->score_trained_user(u, batch);
    model->score(model->fold_in(user_ratings(ds, u)), folded);
    double mean = 0.0;
    for (const float x : batch) mean += x;
    mean /= static_cast<double>(batch.size());
    for (recs::ItemIdx i = 0; i < ds.n_items(); ++i) {
      diff += (batch[i] - folded[i]) * (batch[i] - folded[i]);
      norm += (batch[i] - mean) * (batch[i] - mean);
    }
  }
  const double rel = std::sqrt(diff / norm);
  const double j = mean_parity_jaccard(*model, ds);
  INFO("relative score difference " << rel << ", mean Jaccard@10 " << j);
  CHECK(rel < 1e-2);
  CHECK(j > 0.95);
}

// --- Shuffling -----------------------------------------------------------------------------

TEST_CASE("MF epoch order is a fresh seeded permutation every epoch", "[mf]") {
  const std::size_t n = 200;
  recs::mf_detail::EpochShuffler a(n, 1), b(n, 1), c(n, 2);
  std::vector<std::vector<std::uint32_t>> seen;
  for (int e = 0; e < 5; ++e) {
    const auto oa = a.next();
    const auto ob = b.next();
    const auto oc = c.next();
    CHECK(oa == ob);  // same seed, same stream
    CHECK(oa != oc);  // the seed matters
    auto sorted = oa;
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::uint32_t> iota(n);
    std::iota(iota.begin(), iota.end(), 0u);
    CHECK(sorted == iota);  // a permutation
    CHECK(oa != iota);      // actually shuffled
    for (const auto& prev : seen) CHECK(oa != prev);  // a new order each epoch
    seen.push_back(oa);
  }
}

TEST_CASE("MF record shuffle applies exactly the EpochShuffler order", "[mf]") {
  // fit() shuffles the records in place; after every epoch, record j must be rating order[j].
  const std::size_t n = 1000;
  std::vector<recs::mf_detail::SgdRating> ratings(n);
  for (std::uint32_t j = 0; j < n; ++j) ratings[j] = {j % 37, j, static_cast<float>(j)};
  for (const std::uint64_t seed : {1u, 42u, 9001u}) {
    INFO("seed " << seed);
    recs::mf_detail::EpochShuffler reference(n, seed);
    recs::mf_detail::RecordShuffler shuffler(seed);
    auto records = ratings;
    for (int e = 0; e < 5; ++e) {
      const auto& order = reference.next();
      shuffler.next(records);
      bool same = true;
      for (std::size_t j = 0; j < n; ++j)
        same = same && records[j].row == ratings[order[j]].row && records[j].col == ratings[order[j]].col &&
               records[j].value == ratings[order[j]].value;
      CHECK(same);
    }
  }
}

TEST_CASE("MF sgd_epoch is bit-identical to sgd_step over the same ratings", "[mf]") {
  // Prefetching changes memory access only; the parameters must match exactly, for epochs
  // shorter than, equal to and longer than the prefetch distance, and for an empty one.
  constexpr std::size_t d = recs::mf_detail::kPrefetchAhead;
  recs::Rng rng(77);
  const Params start = random_params(rng, 40, 30, 16);
  for (const std::size_t n : {std::size_t{0}, std::size_t{1}, d - 1, d, d + 1, std::size_t{500}}) {
    INFO(n << " ratings");
    std::vector<recs::Triplet> triplets;
    std::vector<recs::mf_detail::SgdRating> records;
    for (std::size_t j = 0; j < n; ++j) {
      const auto u = static_cast<std::uint32_t>(rng.uniform(40));
      const auto i = static_cast<std::uint32_t>(rng.uniform(30));
      const auto v = static_cast<float>(0.5 * (1 + rng.uniform(10)));
      triplets.push_back({u, i, v, 0});
      records.push_back({u, i, v});
    }
    Params a = start, b = start;
    for (const auto& t : triplets) recs::mf_detail::sgd_step(a, t, 0.01, 0.05);
    recs::mf_detail::sgd_epoch(b, records, 0.01, 0.05);
    CHECK(a.bu == b.bu);
    CHECK(a.bi == b.bi);
    CHECK(a.P == b.P);
    CHECK(a.Q == b.Q);
  }
}

TEST_CASE("MF fit reproduces the pre-optimisation training loop bit for bit", "[mf]") {
  // Golden values recorded from the index-shuffle loop (main at 475d121) with this exact setup:
  // nonzero init (so the factors train, not only the biases) and no validation set (so the
  // curve is train RMSE, whose summation order must not follow the shuffle). Any change to the
  // visiting order, the arithmetic or train_rmse changes these bits. (user_init_sd = init_sd and
// blocks = 1 reproduce that loop's initialisation and serial order.) They assume the project's
  // compiler flags (no -ffast-math or FMA contraction); a deliberate change to the numbers must
  // re-record them and say so.
  const auto s = recs::synthetic::low_rank_explicit(60, 50, 3, 0.1, 0.3, 0.0, 5);
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.seed = 7;
  model->fit(s.train,
             recs::Config{{"k", 8}, {"lambda", 0.05}, {"lr", 0.02}, {"lr_decay", 0.97},
                          {"max_epochs", 6}, {"patience", 3}, {"init_sd", 0.1},
                          {"user_init_sd", 0.1}, {"blocks", 1}},
             ctx);
  const std::vector<std::pair<int, double>> golden_curve{
      {1, 0x1.dce04310b6cc3p-1}, {2, 0x1.d4e5a05a80625p-1}, {3, 0x1.d04fdfbe2545fp-1},
      {4, 0x1.cd10a50c932eep-1}, {5, 0x1.ca719766d96b4p-1}, {6, 0x1.c807520888cfcp-1}};
  CHECK(model->training_curve() == golden_curve);
  // FNV-1a over the bytes of every trained user's scores (score_trained_user, users ascending).
  std::uint64_t h = 1469598103934665603ULL;
  std::vector<float> out(50);
  for (recs::UserIdx u = 0; u < 60; ++u) {
    model->score_trained_user(u, out);
    for (const float x : out) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &x, sizeof bits);
      for (int b = 0; b < 4; ++b) {
        h ^= (bits >> (8 * b)) & 0xffu;
        h *= 1099511628211ULL;
      }
    }
  }
  CHECK(h == 0x6e836923a1d18b3aULL);
}

TEST_CASE("MF fit visits ratings in the seeded per-epoch shuffled order", "[mf]") {
  // init_sd = 0 makes the initial parameters independent of the seed (all zero; with zero factors
  // only the biases move), so any dependence of the result on the seed comes from the update
  // order. Replaying SGD by hand with the EpochShuffler orders must reproduce fit(); replaying in
  // CSR order, with one order reused for both epochs, or with another seed's orders must not.
  const auto s = recs::synthetic::low_rank_explicit(40, 30, 2, 0.1, 0.4, 0.0, 3);
  const auto ds = add_biases(s, 3, true).train;
  const double lr = 0.05, decay = 0.9, lambda = 0.02;
  const std::uint64_t seed = 1234;
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.seed = seed;
  model->fit(ds,
             recs::Config{{"k", 2}, {"lambda", lambda}, {"lr", lr}, {"lr_decay", decay},
                          {"max_epochs", 2}, {"init_sd", 0.0}, {"blocks", 1}},
             ctx);

  std::vector<recs::Triplet> ratings;
  double sum = 0.0;
  for (recs::UserIdx u = 0; u < ds.n_users(); ++u)
    for (const auto& r : user_ratings(ds, u)) {
      ratings.push_back({u, r.item, r.value, 0});
      sum += r.value;
    }
  Params zero;
  zero.k = 2;
  zero.mu = sum / static_cast<double>(ratings.size());
  zero.bu.assign(ds.n_users(), 0.0);
  zero.bi.assign(ds.n_items(), 0.0);
  zero.P.assign(std::size_t{ds.n_users()} * 2, 0.0);
  zero.Q.assign(std::size_t{ds.n_items()} * 2, 0.0);

  const auto replay = [&](const std::vector<std::uint32_t>& e1, const std::vector<std::uint32_t>& e2) {
    Params m = zero;
    for (const auto idx : e1) recs::mf_detail::sgd_step(m, ratings[idx], lr, lambda);
    for (const auto idx : e2) recs::mf_detail::sgd_step(m, ratings[idx], lr * decay, lambda);
    return m;
  };
  // Largest |fit score - replayed prediction| over every (user, item).
  const auto max_gap = [&](const Params& m) {
    std::vector<float> sc(ds.n_items());
    double gap = 0.0;
    for (recs::UserIdx u = 0; u < ds.n_users(); ++u) {
      model->score_trained_user(u, sc);
      for (recs::ItemIdx i = 0; i < ds.n_items(); ++i)
        gap = std::max(gap, std::abs(sc[i] - recs::mf_detail::predict(m, u, i)));
    }
    return gap;
  };

  recs::mf_detail::EpochShuffler sh(ratings.size(), seed);
  const auto o1 = sh.next();
  const auto o2 = sh.next();
  recs::mf_detail::EpochShuffler other(ratings.size(), seed + 1);
  const auto x1 = other.next();
  const auto x2 = other.next();
  std::vector<std::uint32_t> csr(ratings.size());
  std::iota(csr.begin(), csr.end(), 0u);

  CHECK(max_gap(replay(o1, o2)) < 1e-5);   // fit() follows the seeded per-epoch orders
  CHECK(max_gap(replay(csr, csr)) > 1e-3);  // "no shuffle" would be caught
  CHECK(max_gap(replay(o1, o1)) > 1e-3);    // "shuffle once, reuse" would be caught
  CHECK(max_gap(replay(x1, x2)) > 1e-3);    // "shuffle ignores ctx.seed" would be caught
}

// --- Validation edge case ------------------------------------------------------------------

TEST_CASE("MF treats a validation set with no usable ratings as no validation", "[mf]") {
  // Validation ratings whose user or item lies outside the training index space cannot be scored.
  // With none left, fit() runs max_epochs and records train RMSE instead of producing NaN.
  const auto s = recs::synthetic::low_rank_explicit(20, 15, 2, 0.1, 0.5, 0.0, 2);
  std::vector<std::vector<recs::Rating>> validation(25);
  validation[3].push_back({15, 4.0f, 0});   // item out of range
  validation[22].push_back({1, 4.0f, 0});   // user out of range
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.validation = &validation;
  model->fit(s.train, recs::Config{{"k", 2}, {"max_epochs", 4}}, ctx);
  CHECK(model->training_curve_metric() == "train_rmse");
  REQUIRE(model->training_curve().size() == 4);
  for (const auto& [e, v] : model->training_curve()) CHECK(std::isfinite(v));
}

// --- Hostile model files -------------------------------------------------------------------

namespace {

// item_factors.bin layout (format 2): magic[8] | u32 version | u32 n_items | u32 k | f64 mu |
// f32 b_i[n] | u32 n_ratings[n] | f32 Q[n * k]
constexpr std::size_t kOffVersion = 8, kOffNItems = 12, kOffMu = 20, kOffBi = 28;

std::string slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void spit(const std::filesystem::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}
template <class T>
void poke(std::string& bytes, std::size_t off, T value) {
  std::memcpy(bytes.data() + off, &value, sizeof(T));
}

}  // namespace

TEST_CASE("MF load rejects hostile or corrupt model files", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(30, 20, 3, 0.1, 0.4, 0.0, 4);
  auto model = recs::make_mf_sgd();
  model->fit(s.train, recs::Config{{"k", 3}, {"max_epochs", 3}}, recs::TrainContext{});
  recs::testing::TempDir dir("mf-hostile");
  model->save(dir.path());
  const auto bin = dir / "item_factors.bin";
  const auto cfg = dir / "config.json";
  const std::string good_bin = slurp(bin), good_cfg = slurp(cfg);
  REQUIRE(good_bin.size() == 28 + 4 * 20 + 4 * 20 + 4 * 20 * 3);

  const auto rejects = [&](const std::string& b, const std::string& c) {
    spit(bin, b);
    spit(cfg, c);
    auto m = recs::make_mf_sgd();
    bool threw = false;
    try {
      m->load(dir.path());
    } catch (const std::exception&) {
      threw = true;
    }
    return threw;
  };
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  CHECK_FALSE(rejects(good_bin, good_cfg));  // the unmodified files load

  SECTION("bad magic") {
    auto b = good_bin;
    b[0] = 'X';
    CHECK(rejects(b, good_cfg));
  }
  SECTION("huge n_items is rejected by the size check before any allocation") {
    auto b = good_bin;
    poke<std::uint32_t>(b, kOffNItems, 0xFFFFFFFFu);
    CHECK(rejects(b, good_cfg));
  }
  SECTION("non-finite mu") {
    auto b = good_bin;
    poke<double>(b, kOffMu, std::numeric_limits<double>::quiet_NaN());
    CHECK(rejects(b, good_cfg));
  }
  SECTION("non-finite b_i") {
    auto b = good_bin;
    poke<float>(b, kOffBi + 4 * 5, nan);
    CHECK(rejects(b, good_cfg));
  }
  SECTION("non-finite Q") {
    auto b = good_bin;
    poke<float>(b, b.size() - 4, inf);
    CHECK(rejects(b, good_cfg));
  }
  SECTION("format 1 (no per-item rating counts) is rejected") {
    auto b = good_bin;
    poke<std::uint32_t>(b, kOffVersion, 1u);
    CHECK(rejects(b, good_cfg));
  }
  SECTION("trailing bytes") {
    CHECK(rejects(good_bin + '\0', good_cfg));
  }
  SECTION("truncated") {
    CHECK(rejects(good_bin.substr(0, good_bin.size() - 1), good_cfg));
    CHECK(rejects(good_bin.substr(0, 10), good_cfg));
  }
  SECTION("k in the file does not match config.json") {
    auto c = recs::Config::parse(good_cfg);
    c["k"] = 4;
    CHECK(rejects(good_bin, c.dump()));
  }
}

// --- Contract tests -----------------------------------------------------------------------

TEST_CASE("MF fills config defaults from configs/mf.json values", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(30, 20, 2, 0.1, 0.5, 0.0, 1);
  auto model = recs::make_mf_sgd();
  CHECK(model->name() == "mf");
  CHECK(model->predicts_ratings());
  model->fit(s.train, recs::Config{{"max_epochs", 2}}, recs::TrainContext{});
  const auto cfg = model->config();
  CHECK(cfg.at("k") == 128);
  CHECK(cfg.at("lambda") == 0.12);
  CHECK(cfg.at("lr") == 0.02);
  CHECK(cfg.at("lr_decay") == 0.995);
  CHECK(cfg.at("max_epochs") == 2);
  CHECK(cfg.at("patience") == 50);
  CHECK(cfg.at("init_sd") == 0.05);
  CHECK(cfg.at("user_init_sd") == 0.0);
  CHECK(cfg.at("blocks") == 8);
  CHECK(model->n_items() == 20);
  CHECK(model->training_curve().size() == 2);
  CHECK(model->training_curve_metric() == "train_rmse");
}

TEST_CASE("MF rejects invalid configs", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(10, 10, 2, 0.1, 0.5, 0.0, 1);
  for (const auto& bad : {recs::Config{{"k", 0}}, recs::Config{{"lr", -1.0}},
                          recs::Config{{"lambda", -0.1}}, recs::Config{{"lr_decay", 0.0}},
                          recs::Config{{"max_epochs", 0}}, recs::Config{{"user_init_sd", -0.1}},
                          recs::Config{{"min_support", -1}}, recs::Config{{"min_support", 2.5}},
                          recs::Config{{"min_support", 5000000000LL}}, recs::Config{{"blocks", 0}},
                          recs::Config{{"blocks", 257}},
                          recs::Config{{"bogus", 1}}}) {
    INFO(bad.dump());
    auto model = recs::make_mf_sgd();
    CHECK_THROWS_AS(model->fit(s.train, bad, recs::TrainContext{}), std::invalid_argument);
  }
}

TEST_CASE("MF training is bit-identical for a fixed seed and differs across seeds", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(80, 60, 3, 0.1, 0.3, 0.2, 4);
  const recs::Config cfg{{"k", 4}, {"max_epochs", 15}};
  recs::TrainContext ctx;
  ctx.validation = &s.heldout;
  ctx.seed = 99;
  auto a = recs::make_mf_sgd();
  auto b = recs::make_mf_sgd();
  auto c = recs::make_mf_sgd();
  a->fit(s.train, cfg, ctx);
  b->fit(s.train, cfg, ctx);
  ctx.seed = 100;
  c->fit(s.train, cfg, ctx);
  CHECK(a->training_curve() == b->training_curve());
  std::vector<float> sa(60), sb(60), sc(60);
  bool any_diff = false;
  for (recs::UserIdx u = 0; u < 80; ++u) {
    a->score_trained_user(u, sa);
    b->score_trained_user(u, sb);
    c->score_trained_user(u, sc);
    CHECK(sa == sb);
    any_diff = any_diff || sa != sc;
  }
  CHECK(any_diff);
}

TEST_CASE("MF early stopping keeps the best validation epoch", "[mf]") {
  // A large learning rate with no decay makes validation RMSE bottom out and rise again.
  const auto s = recs::synthetic::low_rank_explicit(100, 80, 3, 0.3, 0.2, 0.2, 8);
  auto model = recs::make_mf_sgd();
  recs::TrainContext ctx;
  ctx.validation = &s.heldout;
  model->fit(s.train,
             recs::Config{{"k", 20}, {"lambda", 0.0}, {"lr", 0.05}, {"lr_decay", 1.0},
                          {"max_epochs", 200}, {"patience", 3}, {"init_sd", 0.1}},
             ctx);
  const auto curve = model->training_curve();
  CHECK(model->training_curve_metric() == "val_rmse");
  REQUIRE(curve.size() >= 4);
  CHECK(curve.size() < 200);  // stopped early
  for (std::size_t e = 0; e < curve.size(); ++e) CHECK(curve[e].first == static_cast<int>(e + 1));
  // The best epoch is exactly `patience` epochs before the last one.
  const auto best = std::min_element(curve.begin(), curve.end(),
                                     [](const auto& x, const auto& y) { return x.second < y.second; });
  CHECK(curve.end() - best == 4);
  // The kept parameters are that epoch's: re-measured validation RMSE equals the curve minimum.
  const double rmse = heldout_rmse(*model, s.train, s.heldout, true);
  CHECK(std::abs(rmse - best->second) < 1e-5);
}

TEST_CASE("MF score() is finite, one per item, and explain() cites only rated items", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(60, 50, 3, 0.1, 0.3, 0.0, 12);
  auto model = recs::make_mf_sgd();
  model->fit(s.train, recs::Config{{"k", 4}, {"max_epochs", 20}}, recs::TrainContext{});
  std::vector<float> scores(50);
  recs::testing::for_seeds(30, 5, [&](recs::Rng& rng, std::uint64_t) {
    std::vector<recs::Rating> rs;
    for (recs::ItemIdx i = 0; i < 50; ++i)
      if (rng.uniform01() < 0.2) rs.push_back({i, static_cast<float>(0.5 * (1 + rng.uniform(10))), 0});
    const auto st = model->fold_in(rs);
    CHECK(st.ratings == rs);
    CHECK(st.vec.size() == 4);
    model->score(st, scores);
    for (const float x : scores) CHECK(std::isfinite(x));
    const std::set<recs::ItemIdx> rated = [&] {
      std::set<recs::ItemIdx> r;
      for (const auto& x : rs) r.insert(x.item);
      return r;
    }();
    for (recs::ItemIdx i = 0; i < 50; ++i) {
      const auto ex = model->explain(st, i);
      CHECK(ex.because.size() <= 3);
      for (std::size_t j = 0; j < ex.because.size(); ++j) {
        CHECK(rated.count(ex.because[j].item) == 1);
        CHECK(ex.because[j].weight > 0.0f);
        if (j > 0) CHECK(ex.because[j - 1].weight >= ex.because[j].weight);
      }
    }
  });
  // An empty history folds in to the baseline mu + b_i.
  const auto empty = model->fold_in({});
  CHECK(empty.bias == 0.0);
  model->score(empty, scores);
  for (const float x : scores) CHECK(std::isfinite(x));
  CHECK(model->explain(empty, 0).because.empty());
}

TEST_CASE("MF fold-in rejects unknown items", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(10, 10, 2, 0.1, 0.5, 0.0, 1);
  auto model = recs::make_mf_sgd();
  model->fit(s.train, recs::Config{{"k", 2}, {"max_epochs", 1}}, recs::TrainContext{});
  const std::vector<recs::Rating> bad{{10, 4.0f, 0}};
  CHECK_THROWS_AS(model->fold_in(bad), std::invalid_argument);
}

TEST_CASE("MF save/load round-trips fold-in scores exactly", "[mf]") {
  const auto s = recs::synthetic::low_rank_explicit(50, 40, 3, 0.1, 0.3, 0.0, 21);
  auto model = recs::make_mf_sgd();
  model->fit(s.train, recs::Config{{"k", 3}, {"max_epochs", 10}}, recs::TrainContext{});
  recs::testing::TempDir dir("mf");
  model->save(dir.path());
  auto loaded = recs::make_mf_sgd();
  loaded->load(dir.path());
  CHECK(loaded->n_items() == 40);
  CHECK(loaded->config() == model->config());
  CHECK_FALSE(loaded->supports_trained_scoring());  // per-user factors are not shipped
  std::vector<float> a(40), b(40);
  for (recs::UserIdx u = 0; u < 50; ++u) {
    const auto rs = user_ratings(s.train, u);
    model->score(model->fold_in(rs), a);
    loaded->score(loaded->fold_in(rs), b);
    CHECK(a == b);
  }
  // A truncated file is rejected rather than read short.
  for (const auto& e : std::filesystem::directory_iterator(dir.path()))
    if (e.path().extension() == ".bin") std::filesystem::resize_file(e.path(), 20);
  auto broken = recs::make_mf_sgd();
  CHECK_THROWS(broken->load(dir.path()));
}

// --- Ranking floor (min_support) ----------------------------------------------------------

namespace {

// Synthetic ratings thinned so that item i keeps at most i % 12 of its ratings (in user order):
// training counts then run from 0 to 11, so a floor of 6 removes about half of the catalog and
// some movies have no training ratings at all.
recs::Dataset uneven_items() {
  const auto s = recs::synthetic::low_rank_explicit(80, 60, 3, 0.1, 0.3, 0.0, 31);
  std::vector<recs::Triplet> kept;
  std::vector<std::uint32_t> taken(60, 0);
  for (recs::UserIdx u = 0; u < 80; ++u)
    for (const auto& r : user_ratings(s.train, u))
      if (taken[r.item] < r.item % 12) {
        ++taken[r.item];
        kept.push_back({u, r.item, r.value, r.ts});
      }
  return recs::make_dataset("mf-uneven", 80, 60, std::move(kept));
}

std::vector<std::uint32_t> item_counts(const recs::Dataset& ds) {
  std::vector<std::uint32_t> c(ds.n_items());
  for (recs::ItemIdx i = 0; i < ds.n_items(); ++i) c[i] = static_cast<std::uint32_t>(ds.by_item.row_size(i));
  return c;
}

}  // namespace

// The floor is a ranking rule: recommend() never returns a movie with fewer than min_support
// training ratings, while fit(), fold-in and score() (the predicted ratings behind RMSE) are
// exactly what they are without it.
TEST_CASE("MF min_support: recommend() skips movies below the floor; predictions are unchanged",
          "[mf][gate]") {
  const auto train = uneven_items();
  const auto counts = item_counts(train);
  const std::uint32_t floor = 6;
  const auto below = std::count_if(counts.begin(), counts.end(), [&](std::uint32_t c) { return c < floor; });
  REQUIRE(below >= 5);                                     // the floor bites
  REQUIRE(below <= static_cast<std::ptrdiff_t>(counts.size()) - 20);  // and leaves plenty to rank
  REQUIRE(std::count(counts.begin(), counts.end(), 0u) >= 1);  // and an unrated item exists

  const recs::Config base{{"k", 4}, {"max_epochs", 20}};
  recs::Config with_floor = base;
  with_floor["min_support"] = floor;
  recs::Config no_floor = base;
  no_floor["min_support"] = 0;
  auto plain = recs::make_mf_sgd();
  auto floored = recs::make_mf_sgd();
  plain->fit(train, no_floor, recs::TrainContext{});
  floored->fit(train, with_floor, recs::TrainContext{});
  CHECK(floored->config().at("min_support") == floor);
  CHECK(plain->config().at("min_support") == 0);

  std::vector<float> a(60), b(60), ta(60), tb(60);
  for (recs::UserIdx u = 0; u < 80; ++u) {
    INFO("user " << u);
    const auto rs = user_ratings(train, u);
    const auto sa = plain->fold_in(rs);
    const auto sb = floored->fold_in(rs);
    plain->score(sa, a);
    floored->score(sb, b);
    CHECK(a == b);  // identical training and predictions
    plain->score_trained_user(u, ta);
    floored->score_trained_user(u, tb);
    CHECK(ta == tb);

    // The floored list is the unfloored list with the below-floor movies removed, in order.
    std::vector<recs::ItemIdx> expect;
    for (const auto& x : recs::recommend(*plain, sa, 60))
      if (counts[x.item] >= floor) expect.push_back(x.item);
    std::vector<recs::ItemIdx> got;
    for (const auto& x : recs::recommend(*floored, sb, 60)) {
      CHECK(counts[x.item] >= floor);
      got.push_back(x.item);
    }
    CHECK(got == expect);
  }
}

TEST_CASE("MF min_support 0 ranks every movie, including ones never rated in training", "[mf]") {
  const auto train = uneven_items();
  const auto counts = item_counts(train);
  auto model = recs::make_mf_sgd();
  model->fit(train, recs::Config{{"k", 4}, {"max_epochs", 5}, {"min_support", 0}}, recs::TrainContext{});
  const auto list = recs::recommend(*model, model->fold_in({}), 60);
  CHECK(list.size() == 60);
  bool has_unrated = false;
  for (const auto& x : list) has_unrated = has_unrated || counts[x.item] == 0;
  CHECK(has_unrated);
}

TEST_CASE("MF save/load keeps the min_support floor", "[mf]") {
  const auto train = uneven_items();
  const auto counts = item_counts(train);
  auto model = recs::make_mf_sgd();
  model->fit(train, recs::Config{{"k", 4}, {"max_epochs", 10}, {"min_support", 6}}, recs::TrainContext{});
  recs::testing::TempDir dir("mf-floor");
  model->save(dir.path());
  auto loaded = recs::make_mf_sgd();
  loaded->load(dir.path());
  CHECK(loaded->config() == model->config());
  for (recs::UserIdx u = 0; u < 80; ++u) {
    const auto rs = user_ratings(train, u);
    const auto want = recs::recommend(*model, model->fold_in(rs), 60);
    const auto got = recs::recommend(*loaded, loaded->fold_in(rs), 60);
    CHECK(got == want);
    for (const auto& x : got) CHECK(counts[x.item] >= 6);
  }
}

// --- Parallel SGD (DSGD block grid) -------------------------------------------------------

TEST_CASE("MF block grid puts every rating in the cell of its user and item groups", "[mf][dsgd]") {
  const auto s = recs::synthetic::low_rank_explicit(90, 70, 3, 0.1, 0.3, 0.0, 41);
  std::vector<recs::mf_detail::SgdRating> csr;
  for (recs::UserIdx u = 0; u < 90; ++u)
    for (const auto& r : user_ratings(s.train, u)) csr.push_back({u, r.item, r.value});
  for (const std::uint32_t b : {1u, 3u, 8u}) {
    INFO("b = " << b);
    const auto g = recs::mf_detail::make_block_grid(csr, 90, 70, b, 5);
    REQUIRE(g.cells.size() == std::size_t{b} * b);
    std::size_t total = 0;
    std::vector<std::uint64_t> user_weight(b, 0), item_weight(b, 0);
    for (std::size_t c = 0; c < g.cells.size(); ++c) {
      total += g.cells[c].size();
      for (const auto& r : g.cells[c]) {
        CHECK(std::size_t{g.user_group[r.row]} * b + g.item_group[r.col] == c);
        ++user_weight[g.user_group[r.row]];
        ++item_weight[g.item_group[r.col]];
      }
      // Within a cell the ratings keep CSR order.
      CHECK(std::is_sorted(g.cells[c].begin(), g.cells[c].end(), [](const auto& x, const auto& y) {
        return x.row != y.row ? x.row < y.row : x.col < y.col;
      }));
    }
    CHECK(total == csr.size());
    // Balanced: every group within one user's (item's) worth of ratings of total / b.
    const auto max_user = *std::max_element(user_weight.begin(), user_weight.end());
    const auto min_user = *std::min_element(user_weight.begin(), user_weight.end());
    CHECK(max_user - min_user <= 2 * 70);  // a user has at most 70 ratings
    const auto max_item = *std::max_element(item_weight.begin(), item_weight.end());
    const auto min_item = *std::min_element(item_weight.begin(), item_weight.end());
    CHECK(max_item - min_item <= 2 * 90);  // an item has at most 90 ratings
  }
}

TEST_CASE("MF dsgd_epoch equals running its strata one cell at a time", "[mf][dsgd]") {
  // Cells in a stratum share no rows, so the parallel loop must equal a serial replay of the
  // documented schedule: offsets from the stratum stream, cell (g, (g + o) mod b) shuffled by its
  // own stream, then sgd_epoch. This pins coverage (every cell once per epoch) and the order.
  const auto s = recs::synthetic::low_rank_explicit(90, 70, 3, 0.1, 0.3, 0.0, 41);
  std::vector<recs::mf_detail::SgdRating> csr;
  for (recs::UserIdx u = 0; u < 90; ++u)
    for (const auto& r : user_ratings(s.train, u)) csr.push_back({u, r.item, r.value});
  recs::Rng prng(3);
  const Params start = random_params(prng, 90, 70, 6);
  const std::uint32_t b = 4;
  const std::uint64_t seed = 11;

  auto grid = recs::mf_detail::make_block_grid(csr, 90, 70, b, seed);
  auto grid2 = grid;
  std::vector<recs::mf_detail::RecordShuffler> sh, sh2;
  for (std::size_t c = 0; c < grid.cells.size(); ++c) {
    sh.emplace_back(seed, c);
    sh2.emplace_back(seed, c);
  }
  recs::Rng strata(recs::mix_seed(seed, 4)), strata2(recs::mix_seed(seed, 4));
  Params a = start, r = start;
  for (int e = 0; e < 3; ++e) {
    recs::mf_detail::dsgd_epoch(a, grid, sh, strata, 0.02, 0.05);
    std::vector<std::uint32_t> offsets(b);
    std::iota(offsets.begin(), offsets.end(), 0u);
    strata2.shuffle(offsets.begin(), offsets.end());
    std::vector<int> visits(grid2.cells.size(), 0);
    for (const auto o : offsets)
      for (std::uint32_t g = b; g-- > 0;) {  // reverse order: the order within a stratum is free
        const std::size_t c = std::size_t{g} * b + (g + o) % b;
        ++visits[c];
        sh2[c].next(grid2.cells[c]);
        recs::mf_detail::sgd_epoch(r, grid2.cells[c], 0.02, 0.05);
      }
    CHECK(std::all_of(visits.begin(), visits.end(), [](int v) { return v == 1; }));
  }
  CHECK(a.bu == r.bu);
  CHECK(a.bi == r.bi);
  CHECK(a.P == r.P);
  CHECK(a.Q == r.Q);
}

TEST_CASE("MF parallel training is identical on 1 and 4 threads", "[mf][dsgd]") {
  const auto s = recs::synthetic::low_rank_explicit(120, 80, 3, 0.1, 0.3, 0.2, 9);
  const recs::Config cfg{{"k", 6}, {"max_epochs", 8}, {"blocks", 4}};
  recs::TrainContext ctx;
  ctx.validation = &s.heldout;
  ctx.seed = 5;
  const int saved = omp_get_max_threads();
  omp_set_num_threads(1);
  auto one = recs::make_mf_sgd();
  one->fit(s.train, cfg, ctx);
  omp_set_num_threads(4);
  auto four = recs::make_mf_sgd();
  four->fit(s.train, cfg, ctx);
  omp_set_num_threads(saved);
  CHECK(one->training_curve() == four->training_curve());
  std::vector<float> x(80), y(80);
  for (recs::UserIdx u = 0; u < 120; ++u) {
    one->score_trained_user(u, x);
    four->score_trained_user(u, y);
    CHECK(x == y);
  }
}

TEST_CASE("MF with blocks = 1 trains exactly like the serial loop", "[mf][dsgd]") {
  // One cell holding every rating in CSR order, shuffled by stream 0: the serial algorithm.
  const auto s = recs::synthetic::low_rank_explicit(60, 50, 3, 0.1, 0.3, 0.0, 5);
  std::vector<recs::mf_detail::SgdRating> csr;
  for (recs::UserIdx u = 0; u < 60; ++u)
    for (const auto& r : user_ratings(s.train, u)) csr.push_back({u, r.item, r.value});
  recs::Rng prng(8);
  const Params start = random_params(prng, 60, 50, 4);
  auto grid = recs::mf_detail::make_block_grid(csr, 60, 50, 1, 13);
  std::vector<recs::mf_detail::RecordShuffler> sh;
  sh.emplace_back(13, 0);
  recs::Rng strata(recs::mix_seed(13, 4));
  recs::mf_detail::RecordShuffler serial(13);
  auto records = csr;
  Params a = start, r = start;
  for (int e = 0; e < 3; ++e) {
    recs::mf_detail::dsgd_epoch(a, grid, sh, strata, 0.02, 0.05);
    serial.next(records);
    recs::mf_detail::sgd_epoch(r, records, 0.02, 0.05);
  }
  CHECK(a.P == r.P);
  CHECK(a.Q == r.Q);
  CHECK(a.bu == r.bu);
  CHECK(a.bi == r.bi);
}
