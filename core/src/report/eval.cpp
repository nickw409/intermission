// Offline evaluation (spec: Evaluation protocol) and the rating-task baselines.
//
// evaluate()
// ----------
// For every evaluated user u (ascending UserIdx):
//   1. state = m.fold_in(u's training ratings) — the same code path the website uses.
//   2. list  = recommend(m, state, max(ks)): every item except u's training items (and except
//      what the model's ranking mask removes, e.g. MF's min_support floor), best first.
//   3. Ranking metrics at each k against u's held-out ratings (recs/metrics.hpp: relevant iff
//      rating >= 4.0, graded gain rating - 3.5). Users with no relevant held-out item get empty
//      ranking vectors: they are excluded from ranking-metric means (an all-zero row would only
//      measure how many such users there are), and counted in users_without_relevant.
//   4. mean_pop_rank10: the mean popularity rank (1 = most rated in `train`) of the user's top-10,
//      for every user. NaN if the list is empty (no candidates at all); excluded from the mean.
//   5. Rating task, only for models that predict star ratings: per-user sums of squared and
//      absolute errors of score[item] (score() itself, before any ranking mask) against each
//      held-out rating. If the model cannot score a
//      held-out item (a non-finite score, e.g. a movie with no training ratings), the prediction
//      is the global mean of the training ratings and the rating still counts; the number of such
//      fallbacks is printed to stderr at the end of evaluate().
// Catalog coverage@10 is the share of the whole catalog that appears in at least one user's
// top-10, over every evaluated user.
//
// Fold-in parity (models with a batch path only): for a seeded sample of `parity_users` of the
// evaluated users, Jaccard@10 between recommend() on the trained user scores
// (score_trained_user) and recommend() on fold_in(training ratings), both excluding the user's
// training items. A low value means fold-in does not reproduce what training learned (spec: ALS
// must exceed 0.95). The sample is a partial Fisher-Yates shuffle with recs::Rng(parity_seed).
//
// Determinism: users are scored in parallel with a static OpenMP schedule, each into its own
// slot; every reduction (counts, coverage, parity mean, and everything in make_report) runs
// serially in ascending user order afterwards, so results do not depend on the thread count.
//
// Rating baselines
// ----------------
// With mu the global mean of the training ratings and damping lambda = 10 (the same shrinkage
// MF fold-in uses for the user bias):
//   global_mean:     r̂ = mu
//   item_bias:       r̂ = mu + b_i,         b_i = Σ_{u∈R(i)} (r_ui - mu) / (|R(i)| + lambda)
//   user_bias:       r̂ = mu + b_u,         b_u = Σ_{i∈R(u)} (r_ui - mu) / (|R(u)| + lambda)
//   user_item_bias:  r̂ = mu + b_i + b'_u,  b'_u = Σ_{i∈R(u)} (r_ui - mu - b_i) / (|R(u)| + lambda)
// The damping shrinks the bias of an item or user with few ratings towards 0. Predictions are
// not clipped to [0.5, 5]. Reference: Y. Koren, "Factor in the Neighbors: Scalable and Accurate
// Collaborative Filtering", ACM TKDD 4(1), 2010, sec. 2.1 (baseline predictors).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "recs/eval.hpp"
#include "recs/metrics.hpp"
#include "recs/rng.hpp"

namespace recs {
namespace {

constexpr double kBaselineDamping = 10.0;

std::vector<Rating> training_ratings(const Dataset& train, UserIdx u) {
  const auto items = train.by_user.row_indices(u);
  const auto values = train.by_user.row_values(u);
  const auto ts = train.by_user.row_ts(u);
  std::vector<Rating> out(items.size());
  for (std::size_t j = 0; j < items.size(); ++j) out[j] = {items[j], values[j], ts[j]};
  return out;
}

std::vector<ItemIdx> items_of(const std::vector<Scored>& list) {
  std::vector<ItemIdx> out(list.size());
  for (std::size_t j = 0; j < list.size(); ++j) out[j] = list[j].item;
  return out;
}

void check_users(const Dataset& train, std::span<const std::vector<Rating>> heldout,
                 std::span<const UserIdx> users) {
  for (std::size_t j = 0; j < users.size(); ++j) {
    if (users[j] >= train.n_users() || users[j] >= heldout.size()) {
      throw std::invalid_argument("evaluate: user " + std::to_string(users[j]) + " out of range");
    }
    if (j > 0 && users[j] <= users[j - 1]) throw std::invalid_argument("evaluate: users must be strictly ascending");
  }
}

void check_ks(const std::vector<std::size_t>& ks) {
  bool has10 = false, has50 = false;
  for (std::size_t j = 0; j < ks.size(); ++j) {
    if (ks[j] == 0 || (j > 0 && ks[j] <= ks[j - 1])) {
      throw std::invalid_argument("EvalOptions::ks must be positive and strictly ascending");
    }
    has10 = has10 || ks[j] == 10;
    has50 = has50 || ks[j] == 50;
  }
  if (!has10 || !has50) throw std::invalid_argument("EvalOptions::ks must contain 10 and 50");
}

/// Runs body(j) for j in [0, n) on the OpenMP team with a static schedule. The first exception
/// thrown by any iteration is rethrown on the calling thread after the loop.
template <class Body>
void parallel_for(std::size_t n, Body&& body) {
  std::exception_ptr error;
  const auto count = static_cast<std::int64_t>(n);
#pragma omp parallel for schedule(static)
  for (std::int64_t j = 0; j < count; ++j) {
    try {
      body(static_cast<std::size_t>(j));
    } catch (...) {
#pragma omp critical(recs_eval_error)
      if (!error) error = std::current_exception();
    }
  }
  if (error) std::rethrow_exception(error);
}

/// A view of a model whose score() is the model's batch path for one training user, so the
/// parity check can reuse recommend() unchanged.
class TrainedUserView final : public Model {
 public:
  TrainedUserView(const Model& inner, UserIdx u) : inner_(inner), u_(u) {}
  std::string_view name() const override { return inner_.name(); }
  void fit(const Dataset&, const Config&, const TrainContext&) override { unsupported(); }
  UserState fold_in(std::span<const Rating>) const override { unsupported(); }
  void score(const UserState&, std::span<float> out) const override { inner_.score_trained_user(u_, out); }
  Explanation explain(const UserState&, ItemIdx) const override { unsupported(); }
  void mask_unrankable(std::span<float> scores) const override { inner_.mask_unrankable(scores); }
  void save(const fs::path&) const override { unsupported(); }
  void load(const fs::path&) override { unsupported(); }
  std::uint32_t n_items() const override { return inner_.n_items(); }
  Config config() const override { return inner_.config(); }

 private:
  [[noreturn]] static void unsupported() { throw std::logic_error("TrainedUserView only scores"); }
  const Model& inner_;
  UserIdx u_;
};

std::optional<double> foldin_parity(const Model& m, const Dataset& train, std::span<const UserIdx> users,
                                    const EvalOptions& opt) {
  std::vector<UserIdx> sample(users.begin(), users.end());
  const std::size_t p = std::min(opt.parity_users, sample.size());
  if (p == 0) return std::nullopt;
  if (p < sample.size()) {
    Rng rng(opt.parity_seed);
    for (std::size_t i = 0; i < p; ++i) {
      const std::size_t j = i + static_cast<std::size_t>(rng.uniform(sample.size() - i));
      std::swap(sample[i], sample[j]);
    }
    sample.resize(p);
    std::sort(sample.begin(), sample.end());
  }
  std::vector<double> jac(p, 0.0);
  parallel_for(p, [&](std::size_t j) {
    const UserIdx u = sample[j];
    const UserState st = m.fold_in(training_ratings(train, u));
    const auto via_fold_in = items_of(recommend(m, st, 10));
    const auto via_batch = items_of(recommend(TrainedUserView(m, u), st, 10));
    jac[j] = jaccard_at_k(via_batch, via_fold_in, 10);
  });
  double sum = 0.0;
  for (double v : jac) sum += v;
  return sum / static_cast<double>(p);
}

}  // namespace

EvalResult evaluate(const Model& m, const Dataset& train, std::span<const std::vector<Rating>> heldout,
                    std::span<const UserIdx> users, const EvalOptions& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  check_ks(opt.ks);
  check_users(train, heldout, users);
  if (m.n_items() != train.n_items()) throw std::invalid_argument("evaluate: model and dataset item counts differ");

  const std::size_t kmax = opt.ks.back();
  const std::size_t n_ks = opt.ks.size();
  const auto ranks = popularity_ranks(train.by_item);
  const bool rating_task = m.predicts_ratings();
  const std::uint32_t n_items = m.n_items();
  double global_mean = 0.0;  // fallback prediction for held-out items the model cannot score
  if (rating_task && train.n_ratings() > 0) {
    double total = 0.0;
    for (float v : train.by_user.values) total += v;
    global_mean = total / static_cast<double>(train.n_ratings());
  }
  std::vector<std::uint32_t> fallbacks(users.size(), 0);

  EvalResult res;
  res.has_rating_task = rating_task;
  res.per_user.resize(users.size());
  std::vector<std::vector<ItemIdx>> top10(users.size());

  parallel_for(users.size(), [&](std::size_t j) {
    const UserIdx u = users[j];
    PerUserResult& pu = res.per_user[j];
    pu.user = u;
    const std::span<const Rating> hs = heldout[u];
    const UserState st = m.fold_in(training_ratings(train, u));
    pu.n_train = train.by_user.row_size(u);
    const auto ranked = items_of(recommend(m, st, kmax));

    pu.n_relevant = static_cast<std::uint32_t>(count_relevant(hs));
    if (pu.n_relevant > 0) {
      pu.ndcg_at.resize(n_ks);
      pu.recall_at.resize(n_ks);
      pu.precision_at.resize(n_ks);
      for (std::size_t q = 0; q < n_ks; ++q) {
        pu.ndcg_at[q] = ndcg_at_k(ranked, hs, opt.ks[q]);
        pu.recall_at[q] = recall_at_k(ranked, hs, opt.ks[q]);
        pu.precision_at[q] = precision_at_k(ranked, hs, opt.ks[q]);
      }
      pu.map10 = average_precision_at_k(ranked, hs, 10);
    }

    top10[j].assign(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(10, ranked.size())));
    pu.mean_pop_rank10 = top10[j].empty() ? std::numeric_limits<double>::quiet_NaN()
                                          : mean_popularity_rank({top10[j]}, ranks, 10);

    if (rating_task) {
      std::vector<float> scores(n_items);
      m.score(st, scores);
      for (const Rating& r : hs) {
        if (r.item >= n_items) throw std::invalid_argument("evaluate: held-out item out of range");
        double pred = scores[r.item];
        if (!std::isfinite(pred)) {
          pred = global_mean;
          ++fallbacks[j];
        }
        const double err = static_cast<double>(r.value) - pred;
        pu.sq_err += err * err;
        pu.abs_err += std::abs(err);
        ++pu.n_rated;
      }
    }
  });

  // Serial reductions, ascending user order.
  for (const auto& pu : res.per_user) {
    if (pu.n_relevant == 0) ++res.users_without_relevant;
  }
  res.coverage10 = catalog_coverage(top10, n_items, 10);
  std::uint64_t n_fallbacks = 0;
  for (auto c : fallbacks) n_fallbacks += c;
  if (n_fallbacks > 0) {
    std::cerr << "evaluate: " << m.name() << " could not score " << n_fallbacks
              << " held-out rating(s); predicted the training global mean " << global_mean << " for them\n";
  }
  if (m.supports_trained_scoring()) res.foldin_jaccard10 = foldin_parity(m, train, users, opt);

  res.eval_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

std::vector<RatingBaselineResult> evaluate_rating_baselines(const Dataset& train,
                                                            std::span<const std::vector<Rating>> heldout,
                                                            std::span<const UserIdx> users) {
  const double lambda = kBaselineDamping;
  const std::uint32_t n_items = train.n_items();

  double total = 0.0;
  for (float v : train.by_user.values) total += v;
  const double mu = train.n_ratings() == 0 ? 0.0 : total / static_cast<double>(train.n_ratings());

  std::vector<double> b_item(n_items, 0.0);
  for (ItemIdx i = 0; i < n_items; ++i) {
    double s = 0.0;
    for (float v : train.by_item.row_values(i)) s += v - mu;
    b_item[i] = s / (train.by_item.row_size(i) + lambda);
  }

  std::vector<RatingBaselineResult> out(4);
  out[0].name = "global_mean";
  out[1].name = "user_bias";
  out[2].name = "item_bias";
  out[3].name = "user_item_bias";
  for (auto& r : out) {
    r.sq_err.assign(users.size(), 0.0);
    r.abs_err.assign(users.size(), 0.0);
    r.count.assign(users.size(), 0.0);
  }

  for (std::size_t j = 0; j < users.size(); ++j) {
    const UserIdx u = users[j];
    if (u >= train.n_users() || u >= heldout.size()) throw std::invalid_argument("evaluate_rating_baselines: user out of range");
    double s_user = 0.0, s_user_item = 0.0;
    const auto items = train.by_user.row_indices(u);
    const auto values = train.by_user.row_values(u);
    for (std::size_t q = 0; q < items.size(); ++q) {
      s_user += values[q] - mu;
      s_user_item += values[q] - mu - b_item[items[q]];
    }
    const double denom = static_cast<double>(items.size()) + lambda;
    const double b_user = s_user / denom;
    const double b_user_after_items = s_user_item / denom;

    for (const Rating& r : heldout[u]) {
      if (r.item >= n_items) throw std::invalid_argument("evaluate_rating_baselines: item out of range");
      const double preds[4] = {mu, mu + b_user, mu + b_item[r.item], mu + b_item[r.item] + b_user_after_items};
      for (int b = 0; b < 4; ++b) {
        const double err = static_cast<double>(r.value) - preds[b];
        out[static_cast<std::size_t>(b)].sq_err[j] += err * err;
        out[static_cast<std::size_t>(b)].abs_err[j] += std::abs(err);
        out[static_cast<std::size_t>(b)].count[j] += 1.0;
      }
    }
  }
  return out;
}

}  // namespace recs
