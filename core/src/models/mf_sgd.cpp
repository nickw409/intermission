// Biased matrix factorization trained with stochastic gradient descent ("mf").
//
// Prediction
//   r^_ui = mu + b_u + b_i + p_u . q_i
//   mu is the global mean of the training ratings (fixed, not trained); b_u, b_i are user and item
//   biases; p_u, q_i in R^k are latent factor vectors.
//
// Objective (spec: Models -> Matrix factorization)
//   J = sum over training ratings (u, i, r) of
//         1/2 e_ui^2 + 1/2 lambda (b_u^2 + b_i^2 + |p_u|^2 + |q_i|^2),   e_ui = r - r^_ui.
//   The regulariser sits inside the sum, once per rating, as in Koren et al. (2009): this is the
//   objective whose per-rating gradient SGD follows, so a user with n ratings is effectively
//   penalised by lambda * n. The factor 1/2 only rescales the learning rate.
//
// Per-rating gradient (all partials at the old parameter values)
//   dl/db_u = -e + lambda b_u          dl/db_i = -e + lambda b_i
//   dl/dp_u = -e q_i + lambda p_u      dl/dq_i = -e p_u + lambda q_i
//   SGD step: theta <- theta - lr * dl/dtheta, i.e. the classic
//   b_u += lr (e - lambda b_u), p_u += lr (e q_i - lambda p_u), and so on.
//   mf_detail::gradient() sums exactly these terms, and the tests compare it to central finite
//   differences of mf_detail::objective() (Phase 3 gate) and check sgd_step() against it.
//
// Training
//   - Initialisation: biases 0; item factors N(0, init_sd^2) and user factors N(0, user_init_sd^2)
//     (default user_init_sd = 0, see "Convergence and fold-in parity" below). Users and items with
//     no training ratings get zero factors (they would otherwise keep pure noise forever). The
//     random stream is the same whatever user_init_sd is, so the item factors start identically.
//   - Serial order (blocks = 1): each epoch visits every rating once in a seeded Fisher-Yates
//     order, then multiplies the learning rate by lr_decay. The order is defined by
//     mf_detail::EpochShuffler (one recs::Rng(mix_seed(seed, 2)) stream for the run, each epoch
//     re-shuffling the previous permutation); fit() applies the same swaps to the rating records
//     themselves (mf_detail::RecordShuffler), which yields the same sequence of ratings.
//   - Parallel order (blocks = b > 1, DSGD; Gemulla, Nijkamp, Haas & Sismanis 2011): users and
//     items are each split into b groups of about equal rating count (seeded), which cuts the
//     ratings into b x b cells. Cells (g, h) and (g', h') with g != g' and h != h' share no user
//     and no item, so SGD on them touches disjoint parameters. An epoch runs b "strata": stratum
//     o is the b cells (g, (g + o) mod b), trained in parallel, each cell in its own seeded
//     shuffled order (RecordShuffler stream = cell index). The strata come in a fresh seeded order
//     each epoch (Rng(mix_seed(seed, 4))). Every rating is still visited once per epoch, and
//     because no two threads ever touch the same row, the result depends only on the seed, b and
//     the precision, not on the thread count (no Hogwild-style races). The cost is a less random order than a
//     global shuffle: consecutive updates within a cell share user and item groups. With b = 1
//     there is one cell, and the algorithm is exactly the serial one above. On ml-32m (k 64,
//     lambda 0.05) 8 blocks reach the serial RMSE (0.8166 vs 0.8167) at about 0.8 s per epoch
//     instead of 6.5 s, but early stopping lands before the user vectors settle and fold-in
//     parity drops from 0.920 to 0.893; lambda 0.06 restores it (0.939, RMSE 0.8175). See
//     configs/README.md, "mf".
//   - Speed: the loop is bound by memory latency, not arithmetic (each update reads one random
//     user row and one random item row; on ml-32m P and Q are far larger than the caches).
//     Shuffling 12-byte records in place lets each epoch read the ratings sequentially instead of
//     gathering them through an index, and mf_detail::sgd_epoch prefetches the parameter rows a
//     few ratings ahead. Neither changes any arithmetic: with blocks = 1 and precision "double"
//     training is bit-identical to the plain index-shuffle loop (a golden test pins it). Measured in PR #57 on ml-32m-sized
//     synthetic data (k = 64, one thread): 15.8 s per epoch before, 6.3 s after. Bit-identity
//     assumes the project's flags (no -ffast-math, no -march that enables FMA contraction).
//   - Precision: by default (precision "float") the factor matrices are stored and updated in
//     single precision (biases stay double). Half the bytes per row means half the memory
//     traffic, which is what the loop waits on. Measured on the same ml-32m-sized synthetic data
//     (k = 64, 8 threads, one run): serial double 7.9 s per epoch, 8 blocks double 3.9 s, 8 blocks
//     float 2.2 s. (On real ml-32m 8 blocks float take about 0.8 s per epoch: its ratings
//     concentrate on popular movies, so each cell's item rows stay in cache, which the
//     uniformly random synthetic data does not allow.) Each update's dot product has only k terms, so float accumulation is accurate
//     enough there (see residual()); every sum over ratings (RMSE, objective) stays in double.
//     precision "double" keeps the double-precision reference loop; the gradient gates always
//     check the double formulas (mf_detail::Params). On ml-latest-small validation (configs/mf.json)
//     float and double, 8 blocks, give the same RMSE (0.8955) and fold-in parity (0.916); serial
//     double gives 0.8961 and 0.928.
//   - Early stopping: with a validation set (ctx.validation), the RMSE on it is measured after each
//     epoch; training stops after `patience` epochs without improvement and the parameters of the
//     best epoch are restored. Without one, or when none of its ratings is
//     usable (user or item outside the training index space), training runs max_epochs and the
//     curve is train RMSE.
//   - Determinism: no two threads ever update the same parameter (DSGD above), so training is
//     reproducible for a given seed and blocks value on any thread count. Hogwild-style lock-free
//     parallel SGD would be simpler but is not reproducible, and the spec forbids it. The fitted
//     item factors are saved as float32 whatever the training precision.
//
// Fold-in (spec: "estimate b_u as the damped mean residual, then solve the ridge system for p_u
// with Q and b_i fixed")
//   For a user with ratings r_j on items j (n of them), take the user's part of J with every item
//   parameter fixed:
//     min over b, p of  sum_j 1/2 (r_j - mu - b_j - b - p . q_j)^2 + 1/2 lambda n (b^2 + |p|^2).
//   The spec's recipe is one sweep of block coordinate descent on this convex problem:
//     1. bias with p = 0: b = sum_j (r_j - mu - b_j) / (n + lambda n), the damped mean residual;
//     2. ridge for p given b: (sum_j q_j q_j^T + lambda n I) p = sum_j q_j (r_j - mu - b_j - b).
//   Repeating the two steps converges to the joint minimiser, and we compute that fixed point
//   directly. With z_j = [1, q_j] (length k + 1), s_j = r_j - mu - b_j and x = [b, p]:
//     (sum_j z_j z_j^T + lambda n I) x = sum_j z_j s_j,
//   one Cholesky solve in double precision (recs::solve_spd); a tiny floor on the ridge keeps it
//   positive definite when lambda = 0. Why not stop after one sweep: a training user's (b_u, p_u)
//   is exactly this joint minimiser once SGD has converged, so the joint solve is what makes
//   fold-in agree with the trained vectors. On ml-latest-small trained to convergence
//   (lambda 0.1, 600 epochs) the single sweep reached Jaccard@10 0.906 against trained vectors,
//   the joint solve 0.972.
//   Fold-in is pure: it reads only item-side parameters. Training users can also be scored from
//   their trained p_u, b_u (score_trained_user), which the fold-in parity gate compares against.
//
// Convergence and fold-in parity (Phase 3 gate: Jaccard@10 > 0.9, fold-in vs trained vectors)
//   Parity measures how far SGD got on each user's subproblem. Hold the item side fixed: the user
//   part of J is the quadratic above, with Hessian H_u = sum_j z_j z_j^T + lambda n I. An epoch
//   of SGD visits each of the user's n ratings once, so to first order it is one gradient step
//   x <- x - lr H_u (x - x*), and the error along an eigenvector of H_u with eigenvalue h shrinks
//   by (1 - lr h) per epoch. Every eigenvalue is at least lambda n, and the smallest ones belong to
//   directions the user's rated items barely span, so after T epochs the error that is left is
//   about exp(-lambda n sum_t lr_t). That error is exactly what fold-in (the exact x*) does not
//   have, and the unrated items' q_i do project onto those directions, so the top-10 lists differ.
//   Two sources of error, two fixes:
//     - Initial noise. With p_u ~ N(0, init_sd^2), the noise outside the span of the user's item
//       factors decays only at that slowest rate. Starting user factors at zero
//       (user_init_sd = 0) removes it: an SGD step is p_u <- (1 - lr lambda) p_u + lr e q_j, so p_u
//       is always a combination of the rated items' factors (as they were when visited), which
//       is where the ridge solution lives too. The item factors stay random, which is enough to
//       break the symmetry (the first p_u step is lr e q_j != 0). This is the one-sided start of
//       alternating least squares (Zhou et al. 2008 initialise only the item matrix). On
//       ml-latest-small it lowered validation RMSE by 0.0005-0.001 at equal settings; its effect
//       on parity alone was within run-to-run noise, so the lag below is the larger term.
//     - Lag. Early stopping halts at the validation-RMSE minimum. With a small lambda that
//       minimum comes early, while the slow directions are still far from x*: early stopping is
//       itself doing the regularising (Yao, Rosasco & Caponnetto 2007 show early-stopped
//       gradient descent acts like a ridge penalty). Raising lambda moves the regularisation
//       into the objective: lambda n sum_t lr_t grows, the users converge sooner, and the
//       validation minimum moves to where SGD has (nearly) converged. lambda is still chosen
//       on validation RMSE, subject to parity > 0.9; the search tables are in the commit that
//       set configs/mf.json.
//   On ml-latest-small (users have tens of ratings, so lambda n is small) this costs about
//   0.0015 validation RMSE (0.8946 best overall vs 0.8961 best with parity > 0.9; the bootstrap
//   CI half-width is about 0.02). On ml-32m the many users with few ratings (small lambda n) are
//   the slow ones, and training length matters as much as lambda: an earlier search with
//   shorter training (80-150 epochs) needed lambda 0.1 and RMSE 0.8455 to reach parity 0.9.
//   With lr 0.03, lr_decay 0.995 and up to 800 epochs, lambda 0.06 gives validation RMSE 0.8175
//   and parity 0.939 with 8 blocks (configs/mf.ml-32m.tuned.json; tables in configs/README.md).
//
// Ranking floor (min_support)
//   MF is fit to predict star ratings, and the squared loss says nothing about which movies to put
//   in a top-10. A movie with a handful of ratings has a b_i and q_i fit to that handful: shrunk
//   towards 0 by lambda, but still noisy, and a few enthusiastic ratings make it look like a 4.5
//   for everybody. Ranking the whole catalog by predicted rating therefore fills top-10 lists with
//   obscure movies, which is why MF's NDCG@10 is near zero while its RMSE is good (Koren 2008 and
//   Cremonesi, Koren & Turrin 2010 show the same gap between rating accuracy and top-N accuracy).
//   So MF only recommends movies with at least `min_support` training ratings, the same rule
//   item-KNN uses for its neighbour lists. It is applied in mask_unrankable(), after score():
//   training, fold-in and the predicted ratings (RMSE/MAE, explanations) are unchanged, and only
//   the ranking skips the rarely rated movies. min_support = 0 ranks every movie, including ones
//   with no training ratings (those score the user's baseline mu + b_u). The per-item training
//   counts are saved with the model. min_support is chosen on the validation split per dataset
//   (configs/README.md, "mf").
//
// Explanations
//   x is linear in the residuals, x = W^{-1} sum_j z_j s_j with W = sum_j z_j z_j^T + lambda n I.
//   So the personalised part of the score splits exactly into one term per rated item:
//     b_u + p_u . q_i = x . z_i = sum_j s_j (z_i^T W^{-1} z_j).
//   explain() reports the three largest positive terms (Koren & Bell 2011, "Advances in
//   collaborative filtering", sec. on explaining factorization models via the implied
//   item-item weights).
//
// Scores are predicted star ratings (predicts_ratings() is true). Items never rated in training
// have b_i = 0 and q_i = 0, so they score the user's baseline mu + b_u.
//
// References
//   Y. Koren, R. Bell, C. Volinsky, "Matrix Factorization Techniques for Recommender Systems",
//     IEEE Computer 42(8), 2009.
//   S. Funk, "Netflix Update: Try This at Home", 2006 (the SGD recipe).
//   R. Gemulla, E. Nijkamp, P. J. Haas, Y. Sismanis, "Large-Scale Matrix Factorization with
//     Distributed Stochastic Gradient Descent", KDD 2011 (DSGD, the stratified block schedule).
//   P. Cremonesi, Y. Koren, R. Turrin, "Performance of Recommender Algorithms on Top-N
//     Recommendation Tasks", RecSys 2010 (rating accuracy vs top-N accuracy).
//   Y. Koren, R. Bell, "Advances in Collaborative Filtering", Recommender Systems Handbook, 2011.
//   Y. Yao, L. Rosasco, A. Caponnetto, "On Early Stopping in Gradient Descent Learning",
//     Constructive Approximation 26(2), 2007.
//   Y. Zhou, D. Wilkinson, R. Schreiber, R. Pan, "Large-Scale Parallel Collaborative Filtering for
//     the Netflix Prize", AAIM 2008 (ALS-WR).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>

#include "models/mf_sgd_detail.hpp"
#include "recs/foldin.hpp"
#include "recs/model.hpp"
#include "recs/rng.hpp"

namespace recs {

namespace mf_detail {

namespace {

// Adds scale * d(per-rating loss)/d(theta) for rating (u, i) with residual e into the gradient
// slots. The slots may alias the parameters themselves (sgd_step), so every parameter is read
// before its slot is written: biases are passed by value, and factor f is read then written
// within one loop iteration.
// Factor arithmetic in T (double for the reference and the gates, float for float training); the
// bias terms always in double (the biases are stored in double, see BasicParams).
template <class T>
void add_rating_gradient(T e, double bu, double bi, const T* p, const T* q, std::uint32_t k, T lambda, T scale,
                         double& g_bu, double& g_bi, T* g_p, T* g_q) {
  const double e_d = e, lambda_d = lambda, scale_d = scale;
  g_bu += scale_d * (-e_d + lambda_d * bu);
  g_bi += scale_d * (-e_d + lambda_d * bi);
  for (std::uint32_t f = 0; f < k; ++f) {
    const T pf = p[f];
    const T qf = q[f];
    g_p[f] += scale * (-e * qf + lambda * pf);
    g_q[f] += scale * (-e * pf + lambda * qf);
  }
}

// r - (mu + b_u + b_i + p_u . q_i) in T: the residual the SGD step uses. For T = double this is
// exactly r - predict() (same order of additions). For float, mu + b_u + b_i is formed in double
// and rounded once, and the dot product is accumulated in float: k terms (64-128 in the shipped
// configs), each a product of factors of size ~0.1, so its rounding error (~k * 6e-8 relative)
// is far below the SGD noise. The project's double-accumulation rule targets sums over ratings
// (millions of terms), which stay in double everywhere (RMSE, objective).
template <class T>
T residual(const BasicParams<T>& m, std::uint32_t u, std::uint32_t i, float value) {
  const T* p = m.P.data() + std::size_t{u} * m.k;
  const T* q = m.Q.data() + std::size_t{i} * m.k;
  T dot = 0;
  for (std::uint32_t f = 0; f < m.k; ++f) dot += p[f] * q[f];
  const double base = m.mu + m.bu[u] + m.bi[i];
  return static_cast<T>(value) - (static_cast<T>(base) + dot);
}

double sq_norm(const double* x, std::uint32_t k) {
  double s = 0.0;
  for (std::uint32_t f = 0; f < k; ++f) s += x[f] * x[f];
  return s;
}

}  // namespace

template <class T>
double predict(const BasicParams<T>& m, std::uint32_t u, std::uint32_t i) {
  const T* p = m.P.data() + std::size_t{u} * m.k;
  const T* q = m.Q.data() + std::size_t{i} * m.k;
  double dot = 0.0;
  for (std::uint32_t f = 0; f < m.k; ++f) dot += double{p[f]} * double{q[f]};
  return m.mu + m.bu[u] + m.bi[i] + dot;
}

double objective(const Params& m, std::span<const Triplet> ratings, double lambda) {
  double j = 0.0;
  for (const auto& r : ratings) {
    const double e = r.value - predict(m, r.row, r.col);
    const double reg = m.bu[r.row] * m.bu[r.row] + m.bi[r.col] * m.bi[r.col] +
                       sq_norm(m.P.data() + std::size_t{r.row} * m.k, m.k) +
                       sq_norm(m.Q.data() + std::size_t{r.col} * m.k, m.k);
    j += 0.5 * e * e + 0.5 * lambda * reg;
  }
  return j;
}

Params gradient(const Params& m, std::span<const Triplet> ratings, double lambda) {
  Params g;
  g.k = m.k;
  g.mu = m.mu;
  g.bu.assign(m.bu.size(), 0.0);
  g.bi.assign(m.bi.size(), 0.0);
  g.P.assign(m.P.size(), 0.0);
  g.Q.assign(m.Q.size(), 0.0);
  for (const auto& r : ratings) {
    const std::size_t u = r.row, i = r.col;
    const double e = residual(m, r.row, r.col, r.value);
    add_rating_gradient(e, m.bu[u], m.bi[i], m.P.data() + u * m.k, m.Q.data() + i * m.k, m.k,
                        lambda, 1.0, g.bu[u], g.bi[i], g.P.data() + u * m.k, g.Q.data() + i * m.k);
  }
  return g;
}

template <class T>
void sgd_step(BasicParams<T>& m, const Triplet& rating, double lr, double lambda) {
  const std::size_t u = rating.row, i = rating.col;
  const T e = residual(m, rating.row, rating.col, rating.value);
  T* p = m.P.data() + u * m.k;
  T* q = m.Q.data() + i * m.k;
  add_rating_gradient<T>(e, m.bu[u], m.bi[i], p, q, m.k, static_cast<T>(lambda), static_cast<T>(-lr), m.bu[u],
                         m.bi[i], p, q);
}

template <class T>
void sgd_epoch(BasicParams<T>& m, std::span<const SgdRating> in_order, double lr, double lambda) {
  const std::size_t n = in_order.size();
  const std::uint32_t k = m.k;
  for (std::size_t j = 0; j < n; ++j) {
    if (j + kPrefetchAhead < n) {
      // Hints only (no effect on results): the rows rating j + kPrefetchAhead will touch.
      const SgdRating& ahead = in_order[j + kPrefetchAhead];
      const T* p = m.P.data() + std::size_t{ahead.row} * k;
      const T* q = m.Q.data() + std::size_t{ahead.col} * k;
      // Hints 64 bytes apart, plus the row's last element: rows are not cache-line aligned, so a
      // row can end in one more line. GCC/Clang builtin (the project's compilers).
      constexpr std::uint32_t kPerLine = 64 / sizeof(T);
      for (std::uint32_t f = 0; f < k; f += kPerLine) {
        __builtin_prefetch(p + f, 1);
        __builtin_prefetch(q + f, 1);
      }
      __builtin_prefetch(p + k - 1, 1);
      __builtin_prefetch(q + k - 1, 1);
      __builtin_prefetch(m.bu.data() + ahead.row, 1);
      __builtin_prefetch(m.bi.data() + ahead.col, 1);
    }
    const SgdRating& r = in_order[j];
    sgd_step(m, Triplet{r.row, r.col, r.value, 0}, lr, lambda);
  }
}

template void sgd_step(Params&, const Triplet&, double, double);
template void sgd_step(FloatParams&, const Triplet&, double, double);
template void sgd_epoch(Params&, std::span<const SgdRating>, double, double);
template void sgd_epoch(FloatParams&, std::span<const SgdRating>, double, double);
template double predict(const Params&, std::uint32_t, std::uint32_t);
template double predict(const FloatParams&, std::uint32_t, std::uint32_t);

RecordShuffler::RecordShuffler(std::uint64_t seed, std::uint64_t stream) : rng_(mix_seed(seed, 2, stream)) {}

void RecordShuffler::next(std::span<SgdRating> records) { rng_.shuffle(records.begin(), records.end()); }

std::vector<std::uint32_t> balanced_groups(std::span<const std::uint64_t> weight, std::uint32_t b, Rng& rng) {
  if (b == 0) throw std::invalid_argument("balanced_groups: b must be >= 1");
  const std::size_t n = weight.size();
  std::vector<std::uint32_t> visit(n);
  std::iota(visit.begin(), visit.end(), 0u);
  rng.shuffle(visit.begin(), visit.end());
  std::uint64_t total = 0;
  for (const auto w : weight) total += w;
  std::vector<std::uint32_t> group(n, 0);
  std::uint64_t before = 0;  // weight of the items visited so far
  for (const std::uint32_t i : visit) {
    // Group = which b-th of the total weight this item starts in (exact integer arithmetic; the
    // product is formed in 128 bits, so no weights or b can overflow it).
    const std::uint64_t g = total == 0 ? 0 : static_cast<std::uint64_t>(static_cast<u128>(before) * b / total);
    group[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(g, b - 1));
    before += weight[i];
  }
  return group;
}

BlockGrid make_block_grid(std::span<const SgdRating> csr, std::uint32_t n_users, std::uint32_t n_items,
                          std::uint32_t b, std::uint64_t seed) {
  std::vector<std::uint64_t> per_user(n_users, 0), per_item(n_items, 0);
  for (const auto& r : csr) {
    ++per_user[r.row];
    ++per_item[r.col];
  }
  BlockGrid g;
  g.b = b;
  Rng user_rng(mix_seed(seed, 3, 0)), item_rng(mix_seed(seed, 3, 1));
  g.user_group = balanced_groups(per_user, b, user_rng);
  g.item_group = balanced_groups(per_item, b, item_rng);
  // Count first and reserve, so the cells are allocated once (no growth while `csr` is alive too).
  std::vector<std::size_t> sizes(std::size_t{b} * b, 0);
  for (const auto& r : csr) ++sizes[std::size_t{g.user_group[r.row]} * b + g.item_group[r.col]];
  g.cells.assign(std::size_t{b} * b, {});
  for (std::size_t c = 0; c < sizes.size(); ++c) g.cells[c].reserve(sizes[c]);
  for (const auto& r : csr) g.cells[std::size_t{g.user_group[r.row]} * b + g.item_group[r.col]].push_back(r);
  return g;
}

template <class T>
void dsgd_epoch(BasicParams<T>& m, BlockGrid& grid, std::vector<RecordShuffler>& cell_shufflers, Rng& stratum_rng,
                double lr, double lambda) {
  const std::uint32_t b = grid.b;
  if (cell_shufflers.size() != grid.cells.size() || grid.cells.size() != std::size_t{b} * b ||
      grid.user_group.size() != m.bu.size() || grid.item_group.size() != m.bi.size())
    throw std::invalid_argument("dsgd_epoch: grid, shufflers and parameters do not match");
  std::vector<std::uint32_t> offsets(b);
  std::iota(offsets.begin(), offsets.end(), 0u);
  stratum_rng.shuffle(offsets.begin(), offsets.end());
  for (const std::uint32_t o : offsets) {
    // The b cells of this stratum touch disjoint user rows and disjoint item rows. The body must
    // not throw: an exception leaving an OpenMP region terminates the program.
#pragma omp parallel for schedule(static)
    for (std::uint32_t g = 0; g < b; ++g) {
      const std::size_t c = std::size_t{g} * b + (g + o) % b;
      cell_shufflers[c].next(grid.cells[c]);
      sgd_epoch(m, grid.cells[c], lr, lambda);
    }
  }
}

template void dsgd_epoch(Params&, BlockGrid&, std::vector<RecordShuffler>&, Rng&, double, double);
template void dsgd_epoch(FloatParams&, BlockGrid&, std::vector<RecordShuffler>&, Rng&, double, double);

EpochShuffler::EpochShuffler(std::size_t n, std::uint64_t seed)
    : rng_(mix_seed(seed, 2)), order_(n) {
  std::iota(order_.begin(), order_.end(), 0u);
}

const std::vector<std::uint32_t>& EpochShuffler::next() {
  rng_.shuffle(order_.begin(), order_.end());
  return order_;
}

}  // namespace mf_detail

namespace {

using mf_detail::Params;

// --- configuration ---------------------------------------------------------------------------

struct MfConfig {
  // Defaults match configs/mf.json, which pins every key (chosen on ml-latest-small validation;
  // see the header comment and configs/README.md).
  std::uint32_t k = 128;
  double lambda = 0.12;
  double lr = 0.02;
  double lr_decay = 0.995;
  int max_epochs = 58;
  int patience = 50;
  double init_sd = 0.05;      // item factors
  double user_init_sd = 0.0;  // user factors (0: start inside the span of the rated items' factors)
  std::uint32_t min_support = 0;  // ranking floor: training ratings a movie needs to be recommended
  std::uint32_t blocks = 8;   // DSGD grid side b (1 = serial SGD); results depend on b, not threads
  std::string precision = "float";  // training arithmetic: "float" (fast) or "double" (reference)

  Config to_json() const {
    return Config{{"k", k},           {"lambda", lambda},     {"lr", lr},
                  {"lr_decay", lr_decay}, {"max_epochs", max_epochs}, {"patience", patience},
                  {"init_sd", init_sd},   {"user_init_sd", user_init_sd}, {"min_support", min_support},
                  {"blocks", blocks}, {"precision", precision}};
  }
};

double number(const Config& cfg, const char* key) {
  const auto& v = cfg.at(key);
  if (!v.is_number()) throw std::invalid_argument(std::string("mf config: ") + key + " must be a number");
  return v.get<double>();
}

long long integer(const Config& cfg, const char* key) {
  const auto& v = cfg.at(key);
  if (!v.is_number_integer())
    throw std::invalid_argument(std::string("mf config: ") + key + " must be an integer");
  return v.get<long long>();
}

MfConfig parse_config(const Config& cfg) {
  MfConfig c;
  if (cfg.is_null()) return c;
  if (!cfg.is_object()) throw std::invalid_argument("mf config must be a JSON object");
  static const char* const known[] = {"k",          "lambda",   "lr",      "lr_decay",    "max_epochs",
                                      "patience",   "init_sd",  "user_init_sd", "min_support", "blocks",
                                      "precision"};
  for (const auto& [key, _] : cfg.items())
    if (std::find(std::begin(known), std::end(known), key) == std::end(known))
      throw std::invalid_argument("mf config: unknown key " + key);
  if (cfg.contains("k")) {
    const auto k = integer(cfg, "k");
    if (k < 1 || k > 4096) throw std::invalid_argument("mf config: k must be in [1, 4096]");
    c.k = static_cast<std::uint32_t>(k);
  }
  if (cfg.contains("lambda")) c.lambda = number(cfg, "lambda");
  if (cfg.contains("lr")) c.lr = number(cfg, "lr");
  if (cfg.contains("lr_decay")) c.lr_decay = number(cfg, "lr_decay");
  if (cfg.contains("init_sd")) c.init_sd = number(cfg, "init_sd");
  if (cfg.contains("user_init_sd")) c.user_init_sd = number(cfg, "user_init_sd");
  if (cfg.contains("max_epochs")) c.max_epochs = static_cast<int>(integer(cfg, "max_epochs"));
  if (cfg.contains("patience")) c.patience = static_cast<int>(integer(cfg, "patience"));
  if (cfg.contains("min_support")) {
    const auto m = integer(cfg, "min_support");
    if (m < 0 || m > 0xFFFFFFFFLL) throw std::invalid_argument("mf config: min_support must be in [0, 2^32 - 1]");
    c.min_support = static_cast<std::uint32_t>(m);
  }
  if (cfg.contains("precision")) {
    const auto& v = cfg.at("precision");
    if (!v.is_string() || (v != "float" && v != "double"))
      throw std::invalid_argument("mf config: precision must be \"float\" or \"double\"");
    c.precision = v.get<std::string>();
  }
  if (cfg.contains("blocks")) {
    const auto b = integer(cfg, "blocks");
    if (b < 1 || b > 256) throw std::invalid_argument("mf config: blocks must be in [1, 256]");
    c.blocks = static_cast<std::uint32_t>(b);
  }
  if (!(c.lambda >= 0.0 && std::isfinite(c.lambda))) throw std::invalid_argument("mf config: lambda must be >= 0");
  if (!(c.lr > 0.0 && std::isfinite(c.lr))) throw std::invalid_argument("mf config: lr must be > 0");
  if (!(c.lr_decay > 0.0 && c.lr_decay <= 1.0)) throw std::invalid_argument("mf config: lr_decay must be in (0, 1]");
  if (!(c.init_sd >= 0.0 && std::isfinite(c.init_sd))) throw std::invalid_argument("mf config: init_sd must be >= 0");
  if (!(c.user_init_sd >= 0.0 && std::isfinite(c.user_init_sd)))
    throw std::invalid_argument("mf config: user_init_sd must be >= 0");
  if (c.max_epochs < 1) throw std::invalid_argument("mf config: max_epochs must be >= 1");
  if (c.patience < 1) throw std::invalid_argument("mf config: patience must be >= 1");
  return c;
}

// --- binary I/O (contracts/artifact.md rule 5: little-endian, fixed width, header) ------------

static_assert(std::endian::native == std::endian::little, "artifact I/O assumes a little-endian host");

constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'M', 'F', '0', '1'};
// Format 2 added the per-item training rating counts (the min_support floor); format 1 files are
// rejected (retrain to upgrade).
constexpr std::uint32_t kFormatVersion = 2;
// item_factors.bin: magic[8] | u32 version | u32 n_items | u32 k | f64 mu | f32 b_i[n_items] |
// u32 n_ratings[n_items] (training ratings per item) | f32 Q[n_items * k] (row-major).
// Header: magic, format version, n_items, k.
constexpr std::uint64_t kHeaderBytes = sizeof(kMagic) + 3 * sizeof(std::uint32_t);
// Bounds a servable model must meet, checked by save() and load() (not by fit(): training with
// lambda = 0 is allowed, it just cannot be served). |mu|, |b_i| and every |q_if| are at most
// kMaxAbsParam, and lambda (the fold-in ridge, lambda * n) is in [kMinServeLambda, kMaxServeLambda].
// With those, k <= 4096 and ratings in [0.5, 5], the fold-in solve and every score stay far inside
// float range. Finite alone is not enough: fuzz_model_load_mf found mu = -1.1e297, and lambda = 0
// leaves only the 1e-9 ridge floor, either of which overflows fold-in into NaN scores. Trained
// models are many orders of magnitude inside these bounds (configs/mf.json: lambda 0.12).
constexpr double kMaxAbsParam = 1e6;
constexpr double kMinServeLambda = 1e-6;
constexpr double kMaxServeLambda = 1e6;

bool param_in_range(double x) { return std::abs(x) <= kMaxAbsParam; }  // false for NaN and inf

void check_servable_lambda(double lambda) {
  if (!(lambda >= kMinServeLambda && lambda <= kMaxServeLambda))
    throw std::runtime_error("mf model: lambda must be in [1e-6, 1e6] to be served");
}

template <class T>
void put(std::string& buf, const T& x) {
  char bytes[sizeof(T)];
  std::memcpy(bytes, &x, sizeof(T));
  buf.append(bytes, sizeof(T));
}

class Reader {
 public:
  explicit Reader(std::string data) : data_(std::move(data)) {}
  template <class T>
  T get() {
    if (data_.size() - pos_ < sizeof(T)) throw std::runtime_error("mf model file is truncated");
    T x;
    std::memcpy(&x, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return x;
  }
  bool at_end() const { return pos_ == data_.size(); }
  std::uint64_t size() const { return data_.size(); }

 private:
  std::string data_;
  std::size_t pos_ = 0;
};

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) throw std::runtime_error("cannot write " + path.string());
}

// --- the model -------------------------------------------------------------------------------

class MfSgd final : public Model {
 public:
  std::string_view name() const override { return "mf"; }
  std::uint32_t n_items() const override { return n_items_; }
  Config config() const override { return cfg_.to_json(); }
  bool predicts_ratings() const override { return true; }
  bool supports_trained_scoring() const override { return has_users_; }
  std::vector<std::pair<int, double>> training_curve() const override { return curve_; }
  std::string training_curve_metric() const override { return curve_metric_; }

  void fit(const Dataset& train, const Config& cfg, const TrainContext& ctx) override {
    cfg_ = parse_config(cfg);
    if (cfg_.precision == "double") {
      fit_with<double>(train, ctx);
    } else {
      fit_with<float>(train, ctx);
    }
  }

  // The training loop in precision T (see "Speed" in the header comment).
  template <class T>
  void fit_with(const Dataset& train, const TrainContext& ctx) {
    const std::uint32_t k = cfg_.k;
    const std::uint32_t nu = train.n_users(), ni = train.n_items();

    // Every training rating as a compact record, in CSR order; make_block_grid moves them into
    // the DSGD cells, which the epochs shuffle in place.
    std::vector<mf_detail::SgdRating> order;
    order.reserve(train.n_ratings());
    double sum = 0.0;
    for (std::uint32_t u = 0; u < nu; ++u) {
      const auto items = train.by_user.row_indices(u);
      const auto vals = train.by_user.row_values(u);
      for (std::size_t j = 0; j < items.size(); ++j) {
        order.push_back({u, items[j], vals[j]});
        sum += vals[j];
      }
    }

    mf_detail::BasicParams<T> m;
    m.k = k;
    m.mu = order.empty() ? 0.0 : sum / static_cast<double>(order.size());
    m.bu.assign(nu, 0.0);
    m.bi.assign(ni, 0.0);
    m.P.assign(std::size_t{nu} * k, 0.0);
    m.Q.assign(std::size_t{ni} * k, 0.0);
    Rng init_rng(mix_seed(ctx.seed, 1));
    for (std::uint32_t u = 0; u < nu; ++u)
      if (train.by_user.row_size(u) > 0)
        for (std::uint32_t f = 0; f < k; ++f)
          m.P[std::size_t{u} * k + f] = static_cast<T>(cfg_.user_init_sd * init_rng.normal());
    for (std::uint32_t i = 0; i < ni; ++i)
      if (train.by_item.row_size(i) > 0)
        for (std::uint32_t f = 0; f < k; ++f) m.Q[std::size_t{i} * k + f] = static_cast<T>(cfg_.init_sd * init_rng.normal());

    const auto* validation = ctx.validation;
    // Validation ratings outside the training index space cannot be scored; if none are usable,
    // train as if there were no validation set (max_epochs, train-RMSE curve) instead of NaN.
    if (validation != nullptr && count_usable(*validation, nu, ni) == 0) validation = nullptr;
    curve_metric_ = validation != nullptr ? "val_rmse" : "train_rmse";
    curve_.clear();

    mf_detail::BlockGrid grid = mf_detail::make_block_grid(order, nu, ni, cfg_.blocks, ctx.seed);
    std::vector<mf_detail::SgdRating>().swap(order);  // the cells hold the ratings now
    std::vector<mf_detail::RecordShuffler> shufflers;
    shufflers.reserve(grid.cells.size());
    for (std::size_t c = 0; c < grid.cells.size(); ++c) shufflers.emplace_back(ctx.seed, c);
    Rng stratum_rng(mix_seed(ctx.seed, 4));

    mf_detail::BasicParams<T> best = m;
    double best_rmse = std::numeric_limits<double>::infinity();
    int since_best = 0;
    double lr = cfg_.lr;
    for (int epoch = 1; epoch <= cfg_.max_epochs; ++epoch) {
      mf_detail::dsgd_epoch(m, grid, shufflers, stratum_rng, lr, cfg_.lambda);
      lr *= cfg_.lr_decay;

      const double rmse = validation != nullptr ? validation_rmse(m, *validation) : train_rmse(m, train);
      if (!std::isfinite(rmse))
        throw std::runtime_error("mf: training diverged at epoch " + std::to_string(epoch) +
                                 " (lower lr or raise lambda)");
      curve_.emplace_back(epoch, rmse);
      if (validation == nullptr) continue;
      if (rmse < best_rmse) {
        best_rmse = rmse;
        best = m;
        since_best = 0;
      } else if (++since_best >= cfg_.patience) {
        break;
      }
    }
    if (validation != nullptr) m = std::move(best);

    mu_ = m.mu;
    n_users_ = nu;
    n_items_ = ni;
    bu_.assign(m.bu.begin(), m.bu.end());
    bi_.assign(m.bi.begin(), m.bi.end());
    n_ratings_.resize(ni);
    for (std::uint32_t i = 0; i < ni; ++i) n_ratings_[i] = static_cast<std::uint32_t>(train.by_item.row_size(i));
    P_.assign(m.P.begin(), m.P.end());
    Q_.assign(m.Q.begin(), m.Q.end());
    has_users_ = true;
  }

  UserState fold_in(std::span<const Rating> ratings) const override {
    const std::uint32_t k = cfg_.k;
    UserState st;
    st.ratings.assign(ratings.begin(), ratings.end());
    st.vec.assign(k, 0.0f);
    if (ratings.empty()) return st;

    // Joint ridge solve for x = [b_u, p_u] (see the header comment).
    const auto x = solve_user(ratings, std::nullopt);
    st.bias = x[0];
    for (std::uint32_t f = 0; f < k; ++f) st.vec[f] = static_cast<float>(x[f + 1]);
    // Defensive: the save/load bounds keep this finite; never hand score() an inf/NaN state.
    if (!std::isfinite(st.bias)) throw std::runtime_error("mf: fold-in produced a non-finite user bias");
    for (const float v : st.vec)
      if (!std::isfinite(v)) throw std::runtime_error("mf: fold-in produced a non-finite user vector");
    return st;
  }

  void score(const UserState& u, std::span<float> out) const override {
    if (u.vec.size() != cfg_.k) throw std::invalid_argument("mf score: user vector has the wrong size");
    score_with(u.bias, u.vec.data(), out);
  }

  // The min_support floor (see "Ranking floor" above); recommend() calls this after score().
  void mask_unrankable(std::span<float> scores) const override {
    if (scores.size() != n_items_) throw std::invalid_argument("mf mask: scores.size() != n_items");
    for (std::uint32_t i = 0; i < n_items_; ++i)
      if (n_ratings_[i] < cfg_.min_support) scores[i] = -std::numeric_limits<float>::infinity();
  }

  void score_trained_user(UserIdx u, std::span<float> out) const override {
    if (!has_users_) throw NotImplemented("mf: per-user factors are not available after load()");
    if (u >= n_users_) throw std::out_of_range("mf score_trained_user: user out of range");
    score_with(bu_[u], P_.data() + std::size_t{u} * cfg_.k, out);
  }

  Explanation explain(const UserState& u, ItemIdx item) const override {
    Explanation ex;
    if (item >= n_items_) throw std::invalid_argument("mf explain: unknown item");
    if (u.ratings.empty()) return ex;
    const std::uint32_t k = cfg_.k;

    // w = W^{-1} z_item; the term for rated item j is s_j (w . z_j), with z = [1, q].
    const auto w = solve_user(u.ratings, item);
    std::vector<Because> terms;
    for (const auto& r : u.ratings) {
      const double s = r.value - mu_ - bi_[r.item];
      double wz = w[0];
      for (std::uint32_t f = 0; f < k; ++f) wz += w[f + 1] * Q_[std::size_t{r.item} * k + f];
      const auto c = static_cast<float>(s * wz);
      if (c > 0.0f) terms.push_back({r.item, c});  // positive after rounding to float
    }
    std::sort(terms.begin(), terms.end(), [](const Because& a, const Because& b) {
      if (a.weight != b.weight) return a.weight > b.weight;
      return a.item < b.item;
    });
    if (terms.size() > 3) terms.resize(3);
    ex.because = std::move(terms);
    return ex;
  }

  void save(const fs::path& dir) const override {
    // The bounds load() enforces, so an unservable model fails here, at train time.
    check_servable_lambda(cfg_.lambda);
    if (!param_in_range(mu_) || !std::all_of(bi_.begin(), bi_.end(), param_in_range) ||
        !std::all_of(Q_.begin(), Q_.end(), param_in_range))
      throw std::runtime_error("mf model: parameters non-finite or out of range; not saved");
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    put(buf, kFormatVersion);
    put(buf, n_items_);
    put(buf, cfg_.k);
    put(buf, mu_);
    for (const float x : bi_) put(buf, x);
    for (const std::uint32_t c : n_ratings_) put(buf, c);
    for (const float x : Q_) put(buf, x);
    write_file(dir / "item_factors.bin", buf);
    write_file(dir / "config.json", cfg_.to_json().dump(2) + "\n");
  }

  void load(const fs::path& dir) override {
    const MfConfig cfg = parse_config(Config::parse(read_file(dir / "config.json")));
    Reader in(read_file(dir / "item_factors.bin"));
    char magic[sizeof(kMagic)];
    for (char& c : magic) c = in.get<char>();
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) throw std::runtime_error("mf model file: bad magic");
    const auto version = in.get<std::uint32_t>();
    if (version == 1)
      throw std::runtime_error("mf model file: format 1 has no per-item rating counts (min_support); retrain");
    if (version != kFormatVersion) throw std::runtime_error("mf model file: unknown version");
    const auto ni = in.get<std::uint32_t>();
    const auto k = in.get<std::uint32_t>();
    if (k != cfg.k) throw std::runtime_error("mf model file: k does not match config.json");
    check_servable_lambda(cfg.lambda);
    // Check the exact length implied by the header before allocating anything from it, so a
    // corrupt or hostile n_items cannot trigger a huge allocation. No overflow: ni < 2^32 and
    // k <= 4096 (parse_config), so the products stay far below 2^64.
    const std::uint64_t expected = kHeaderBytes + sizeof(double) + sizeof(float) * std::uint64_t{ni} +
                                   sizeof(std::uint32_t) * std::uint64_t{ni} + sizeof(float) * std::uint64_t{ni} * k;
    if (in.size() != expected) throw std::runtime_error("mf model file: wrong length for its header");
    const auto mu = in.get<double>();
    // Every parameter within kMaxAbsParam (see the comment there).
    if (!param_in_range(mu)) throw std::runtime_error("mf model file: mu non-finite or out of range");
    std::vector<float> bi(ni), Q(std::size_t{ni} * k);
    std::vector<std::uint32_t> counts(ni);
    for (auto& x : bi) x = in.get<float>();
    for (auto& c : counts) c = in.get<std::uint32_t>();  // any count is valid
    for (auto& x : Q) x = in.get<float>();
    if (!std::all_of(bi.begin(), bi.end(), param_in_range) || !std::all_of(Q.begin(), Q.end(), param_in_range))
      throw std::runtime_error("mf model file: non-finite or out-of-range item parameters");
    if (!in.at_end()) throw std::runtime_error("mf model file: trailing bytes");

    cfg_ = cfg;
    mu_ = mu;
    n_items_ = ni;
    bi_ = std::move(bi);
    n_ratings_ = std::move(counts);
    Q_ = std::move(Q);
    n_users_ = 0;
    bu_.clear();
    P_.clear();
    has_users_ = false;
    curve_.clear();
    curve_metric_.clear();
  }

 private:
  // Validation ratings validation_rmse() can score: user < nu and item < ni.
  static std::size_t count_usable(const std::vector<std::vector<Rating>>& v, std::uint32_t nu,
                                  std::uint32_t ni) {
    std::size_t n = 0;
    for (std::size_t u = 0; u < std::min<std::size_t>(v.size(), nu); ++u)
      for (const auto& r : v[u]) n += r.item < ni ? 1 : 0;
    return n;
  }

  template <class T>
  double validation_rmse(const mf_detail::BasicParams<T>& m, const std::vector<std::vector<Rating>>& validation) const {
    double sse = 0.0;
    std::size_t n = 0;
    const auto nu = static_cast<std::uint32_t>(std::min<std::size_t>(validation.size(), m.bu.size()));
    for (std::uint32_t u = 0; u < nu; ++u)
      for (const auto& r : validation[u]) {
        if (r.item >= m.bi.size()) continue;
        const double e = r.value - mf_detail::predict(m, u, r.item);
        sse += e * e;
        ++n;
      }
    return std::sqrt(sse / static_cast<double>(n));
  }

  // Over the training matrix in CSR order (user, then item ascending), so the sum's order never
  // depends on the epoch shuffle.
  template <class T>
  static double train_rmse(const mf_detail::BasicParams<T>& m, const Dataset& train) {
    if (train.n_ratings() == 0) return 0.0;
    double sse = 0.0;
    for (std::uint32_t u = 0; u < train.n_users(); ++u) {
      const auto items = train.by_user.row_indices(u);
      const auto vals = train.by_user.row_values(u);
      for (std::size_t j = 0; j < items.size(); ++j) {
        const double e = vals[j] - mf_detail::predict(m, u, items[j]);
        sse += e * e;
      }
    }
    return std::sqrt(sse / static_cast<double>(train.n_ratings()));
  }

  // lambda * n (the per-rating penalty summed over the user's n ratings), floored so that the
  // system stays positive definite even with lambda = 0 and fewer ratings than factors.
  double ridge(std::size_t n) const { return std::max(cfg_.lambda * static_cast<double>(n), 1e-9); }

  // The user's normal equations W x = rhs with z_j = [1, q_j], s_j = r_j - mu - b_j:
  //   W = sum_j z_j z_j^T + ridge * I,   rhs = sum_j z_j s_j   ((k+1) x (k+1), double precision).
  // Returns x = [b_u, p_u] when `explain_item` is empty, or W^{-1} z_item for explanations.
  std::vector<double> solve_user(std::span<const Rating> ratings,
                                 std::optional<ItemIdx> explain_item) const {
    const std::uint32_t k = cfg_.k;
    const std::size_t d = std::size_t{k} + 1;
    std::vector<double> W(d * d, 0.0), rhs(d, 0.0), z(d, 1.0);
    for (const auto& r : ratings) {
      if (r.item >= n_items_) throw std::invalid_argument("mf: unknown item " + std::to_string(r.item));
      for (std::uint32_t f = 0; f < k; ++f) z[f + 1] = Q_[std::size_t{r.item} * k + f];
      const double s = r.value - mu_ - bi_[r.item];
      for (std::size_t a = 0; a < d; ++a) {
        rhs[a] += s * z[a];
        for (std::size_t b = 0; b < d; ++b) W[a * d + b] += z[a] * z[b];
      }
    }
    if (explain_item) {
      for (std::uint32_t f = 0; f < k; ++f) z[f + 1] = Q_[std::size_t{*explain_item} * k + f];
      rhs = z;
    }
    return solve_spd(W, rhs, d, ridge(ratings.size()));
  }

  void score_with(double bias, const float* p, std::span<float> out) const {
    if (out.size() != n_items_) throw std::invalid_argument("mf score: out.size() != n_items");
    const std::uint32_t k = cfg_.k;
    for (std::uint32_t i = 0; i < n_items_; ++i) {
      const float* q = Q_.data() + std::size_t{i} * k;
      double dot = 0.0;
      for (std::uint32_t f = 0; f < k; ++f) dot += double{p[f]} * q[f];
      out[i] = static_cast<float>(mu_ + bias + bi_[i] + dot);
    }
  }

  MfConfig cfg_;
  double mu_ = 0.0;
  std::uint32_t n_users_ = 0;
  std::uint32_t n_items_ = 0;
  std::vector<float> bu_, bi_;  // biases
  std::vector<std::uint32_t> n_ratings_;  // training ratings per item (the min_support floor)
  std::vector<float> P_, Q_;    // row-major factors, n_users x k and n_items x k
  bool has_users_ = false;
  std::vector<std::pair<int, double>> curve_;
  std::string curve_metric_;
};

}  // namespace

std::unique_ptr<Model> make_mf_sgd() { return std::make_unique<MfSgd>(); }

}  // namespace recs
