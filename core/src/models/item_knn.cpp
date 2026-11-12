// Item-based k-nearest-neighbour recommender ("knn").
//
// THE IDEA
//   Two movies are similar if the same people rated them in the same direction relative to their
//   own taste. To score a movie i for a user, look at the movies the user already rated that are
//   among i's nearest neighbours, and add up how much the user liked each of them compared with
//   their own average, weighted by how similar each one is to i.
//
// SIMILARITY: ADJUSTED COSINE (Sarwar et al. 2001, section 3.1.3)
//   Centre every rating by its USER's mean, so a harsh rater's 3 and a generous rater's 4 can mean
//   the same thing:            c_uj = r_uj - mu_u,    mu_u = mean of all of u's training ratings.
//   For items i, j let U_ij be the users who rated both ("co-raters") and n_ij = |U_ij|. Then
//
//                       sum_{u in U_ij} c_ui c_uj
//     s_ij = ---------------------------------------------------
//            sqrt(sum_{u in U_ij} c_ui^2) sqrt(sum_{u in U_ij} c_uj^2)
//
//   All three sums run over the co-raters only (Sarwar's definition), not over everyone who rated
//   i or j. By Cauchy-Schwarz s_ij is in [-1, 1]; we clamp the floating-point result to that range
//   so rounding can never push it a hair outside. If either norm is zero (every co-rater rated the
//   item exactly at their own mean) the similarity is defined as 0.
//
// SHRINKAGE (Koren 2008, section 2.2; Bell & Koren 2007)
//   A similarity built from two co-raters is mostly noise, yet it can be +-1. We pull it towards 0
//   in proportion to how little evidence it has:
//                                 s'_ij = s_ij * n_ij / (n_ij + lambda)
//   n/(n + lambda) is in [0, 1], so |s'| <= |s| always (shrinkage never increases a similarity).
//
// NEIGHBOURHOODS
//   Only items with at least `min_support` training ratings take part: they get neighbour lists
//   and only they may appear in someone's list. Zero similarities are always dropped.
//   Option `positive_only` (default true): drop every s' <= 0, then keep the top M = `neighbors`
//   of the rest by s' (ties broken by ItemIdx ascending). "Users who hated X tend to like i" is
//   weak evidence next to "users who liked X liked i", and most top-N implementations keep only
//   positive neighbours (Deshpande & Karypis 2004).
//   With positive_only = false each item keeps its top M partners by |s'| instead, negative ones
//   included. That is the rating-prediction setting: there a user who disliked a movie that is
//   anti-correlated with i is evidence FOR i, and the normalised score below uses it that way.
//
// SCORE: TWO FORMS
//   With N(i) the neighbour list of i, R(u) the items u rated and c_uj = r_uj - mu_u:
//
//   (1) Top-N form, the default (normalize = false; spec "Item-KNN"; Cremonesi et al. 2010):
//
//         score_ui = sum_{j in N(i) and R(u)} s'_ij c_uj
//
//   (2) Rating-prediction form (normalize = true, usually with positive_only = false; Sarwar et
//       al. 2001, section 3.2.1, in mean-centred form; Desrosiers & Karypis 2011):
//
//                     sum_{j in N(i) and R(u)} s'_ij c_uj
//         score_ui = -------------------------------------
//                     sum_{j in N(i) and R(u)} |s'_ij|
//
//       This is the predicted deviation from the user's mean (add mu_u to read it as a star
//       rating; we do not, since a per-user constant changes nothing in the ranking).
//
//   Why the top-N form is the default. Form (2) answers "what would this user rate i?", and it
//   answers it well, but it is a poor answer to "which movies should we show first?". Its value
//   does not depend on how much evidence there is: an obscure item whose single rated neighbour
//   the user loved gets the maximum possible score (that neighbour's c_uj), ahead of a movie that
//   ten of the user's favourites point to. Lists fill up with such items. Form (1) drops the
//   denominator, so the score grows with the amount of agreeing evidence, which is what a ranking
//   needs. Cremonesi, Koren and Turrin (2010) found the non-normalised neighbourhood score far
//   stronger for top-N recommendation, and Deshpande and Karypis (2004) rank by plain similarity
//   sums. Our own numbers agree (docs/phase-1.md): on ml-32m, form (2) with the spec's starting
//   values scores NDCG@10 0.0039, well below the rating-count baseline's 0.0440, while form (1)
//   with positive neighbours and validation-tuned M and lambda scores 0.0726. Form (2) stays as a
//   config option so the two can be compared.
//
//   Tuning note: with form (1) the best shrinkage is large (lambda = 30,000 on ml-32m), so s' is
//   dominated by the co-rating count n_ij and the model behaves much like a "people who rated X
//   also rated Y" co-occurrence ranker. M and lambda are chosen on the validation split for each
//   dataset: M = 10, lambda = 1,000 on ml-latest-small (configs/knn.json, also the code defaults)
//   and M = 50, lambda = 30,000 on ml-32m (configs/knn.ml-32m.tuned.json).
//
//   THE SCORING MEAN. Both forms centre the user's ratings, c_uj = r_uj - mu~_u. With the plain
//   mean (mu~_u = mu_u) a user whose ratings are all equal has c_uj = 0 everywhere: every score is
//   exactly 0, and the list falls back to the ItemIdx tie-break. That is a common new-user profile
//   on the website (rate five favourites, all 5 stars). Option `mean_damping` = beta pulls the
//   scoring mean towards the training set's global mean g with beta pseudo-ratings (the same
//   damping as the popularity model's damped mean; Koren 2008, section 2.1, regularises the user
//   and item baselines towards the global mean in the same spirit):
//
//         mu~_u = (sum_{j in R(u)} r_uj + beta g) / (|R(u)| + beta)
//
//   A 5-star-only user then has c_uj = 5 - mu~_u > 0, and the score ranks by summed similarity to
//   the movies they rated. For a training user with hundreds of ratings mu~_u is nearly mu_u.
//   Only scoring (and explanations) use mu~; the similarities keep the plain mean, which is what
//   adjusted cosine is defined with. beta = 0 is the plain mean.
//
//   In both forms, if none of i's neighbours was rated, score_ui = -infinity: the item cannot be
//   recommended, so both forms rank the same candidate set. Sums run in double precision in the
//   stored neighbour order, so the result is fully deterministic.
//
// EXPLANATIONS
//   explain(u, i) lists the rated neighbours j whose term in the score is positive, largest first
//   (at most 3), with weight = that term, i.e. each one's actual share of score_ui:
//     normalize = false:  weight_j = s'_ij (r_uj - mu~_u)
//     normalize = true:   weight_j = s'_ij (r_uj - mu~_u) / sum |s'|
//   In both modes the score is the sum of all such terms, positive and negative (up to rounding).
//
// FOLD-IN AND THE BATCH PATH
//   Everything item-side (the neighbour lists) is fixed after fit(). A user is just
//   {ratings sorted by item, mu~_u}. fold_in() computes mu~_u by summing the values in ascending
//   item order; fit() computes each training user's mean with the same function over the training row
//   (which is also in ascending item order), and score_trained_user() then runs the very same
//   scoring routine. The two paths are therefore bit-identical, which a gate test checks.
//
// COMPUTING THE SIMILARITIES (cost and determinism)
//   For each item i we walk its raters u (item x user matrix), and for each one walk u's other
//   ratings j (user x item matrix), accumulating the three sums and n_ij for every j in dense
//   per-thread arrays. That costs sum over users of n_u^2 multiply-adds overall, never builds the
//   n_items x n_items matrix, and needs O(n_items) scratch per thread. Every item's row depends
//   only on the data, and within a row each sum adds its terms in ascending user order, so the
//   result is the same for any thread count and schedule. The same order makes s_ij and s_ji the
//   same floating-point computation (a*b == b*a exactly in IEEE arithmetic), so the similarity is
//   EXACTLY symmetric, not merely within a tolerance.
//
// REFERENCES
//   B. Sarwar, G. Karypis, J. Konstan, J. Riedl. "Item-based collaborative filtering
//     recommendation algorithms." WWW 2001.
//   R. Bell, Y. Koren. "Improved neighborhood-based collaborative filtering." KDD Cup 2007.
//   Y. Koren. "Factorization meets the neighborhood: a multifaceted collaborative filtering
//     model." KDD 2008.
//   P. Cremonesi, Y. Koren, R. Turrin. "Performance of recommender algorithms on top-N
//     recommendation tasks." RecSys 2010.
//   M. Deshpande, G. Karypis. "Item-based top-N recommendation algorithms." ACM TOIS 22(1), 2004.
//   C. Desrosiers, G. Karypis. "A comprehensive survey of neighborhood-based recommendation
//     methods." Recommender Systems Handbook, 2011.
//
// ARTIFACT (save/load): one file `item_knn.bin`, little-endian, see save() below.
#include <omp.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "models/item_knn_detail.hpp"
#include "recs/model.hpp"
#include "sparse/offsets.hpp"

namespace recs {

// ---------------------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------------------
namespace item_knn_detail {

namespace {

std::uint32_t read_count(const Config& v, const char* key, std::uint32_t min_value) {
  if (!v.is_number_integer()) throw std::invalid_argument(std::string("knn: '") + key + "' must be an integer");
  if (v.is_number_unsigned()) {
    const auto x = v.get<std::uint64_t>();
    if (x >= min_value && x <= std::numeric_limits<std::uint32_t>::max()) return static_cast<std::uint32_t>(x);
  } else {
    const auto x = v.get<std::int64_t>();
    if (x >= static_cast<std::int64_t>(min_value) && x <= std::numeric_limits<std::uint32_t>::max()) {
      return static_cast<std::uint32_t>(x);
    }
  }
  throw std::invalid_argument(std::string("knn: '") + key + "' must be >= " + std::to_string(min_value));
}

}  // namespace

Params parse_config(const Config& cfg) {
  Params p;
  if (cfg.is_null()) return p;
  if (!cfg.is_object()) throw std::invalid_argument("knn: config must be a JSON object");
  for (const auto& [key, value] : cfg.items()) {
    if (key == "neighbors") {
      p.neighbors = read_count(value, "neighbors", 1);
    } else if (key == "min_support") {
      p.min_support = read_count(value, "min_support", 1);
    } else if (key == "shrinkage") {
      if (!value.is_number()) throw std::invalid_argument("knn: 'shrinkage' must be a number");
      p.shrinkage = value.get<double>();
      if (!std::isfinite(p.shrinkage) || p.shrinkage < 0.0) {
        throw std::invalid_argument("knn: 'shrinkage' must be finite and >= 0");
      }
    } else if (key == "mean_damping") {
      if (!value.is_number()) throw std::invalid_argument("knn: 'mean_damping' must be a number");
      p.mean_damping = value.get<double>();
      if (!std::isfinite(p.mean_damping) || p.mean_damping < 0.0) {
        throw std::invalid_argument("knn: 'mean_damping' must be finite and >= 0");
      }
    } else if (key == "normalize" || key == "positive_only") {
      if (!value.is_boolean()) throw std::invalid_argument("knn: '" + key + "' must be true or false");
      (key == "normalize" ? p.normalize : p.positive_only) = value.get<bool>();
    } else {
      throw std::invalid_argument("knn: unknown config key '" + key + "'");
    }
  }
  return p;
}

Config to_config(const Params& p) {
  return Config{{"neighbors", p.neighbors},
                {"shrinkage", p.shrinkage},
                {"min_support", p.min_support},
                {"normalize", p.normalize},
                {"positive_only", p.positive_only},
                {"mean_damping", p.mean_damping}};
}

}  // namespace item_knn_detail

namespace {

using item_knn_detail::PairSimilarity;
using item_knn_detail::Params;

constexpr float kCannotScore = -std::numeric_limits<float>::infinity();

/// mu_u: the plain mean of a user's ratings, summed in double in the order given. Both fold_in()
/// and fit() call this with the values in ascending item order, which is what makes the fold-in
/// and batch paths bit-identical.
double user_mean(std::span<const float> values_by_item) {
  if (values_by_item.empty()) return 0.0;
  double sum = 0.0;
  for (float v : values_by_item) sum += static_cast<double>(v);
  return sum / static_cast<double>(values_by_item.size());
}

/// The scoring mean: mu_u pulled towards the training set's global mean g by beta pseudo-ratings,
///   (sum r + beta g) / (n + beta).
/// beta = 0 is the plain mean. Summed in double in the order given (ascending item order on both
/// the fold-in and the batch path, so the two stay bit-identical).
double damped_mean(std::span<const float> values_by_item, double beta, double global_mean) {
  if (beta == 0.0) return user_mean(values_by_item);
  double sum = 0.0;
  for (float v : values_by_item) sum += static_cast<double>(v);
  return (sum + beta * global_mean) / (static_cast<double>(values_by_item.size()) + beta);
}

/// A light CSR (no timestamps): rows -> (column, raw rating).
struct Rows {
  std::vector<std::uint64_t> indptr{0};
  std::vector<std::uint32_t> cols;
  std::vector<float> values;

  std::size_t n_rows() const { return indptr.size() - 1; }
  std::uint64_t begin(std::size_t r) const { return indptr[r]; }
  std::uint64_t end(std::size_t r) const { return indptr[r + 1]; }
};

/// Copies `m` keeping only entries whose COLUMN passes `keep_col` (all rows kept, possibly empty).
template <class Keep>
Rows filter_columns(const Csr& m, Keep keep_col) {
  Rows out;
  out.indptr.reserve(static_cast<std::size_t>(m.n_rows) + 1);
  for (std::uint32_t r = 0; r < m.n_rows; ++r) {
    for (std::uint64_t k = m.indptr[r]; k < m.indptr[r + 1]; ++k) {
      if (keep_col(m.indices[k])) {
        out.cols.push_back(m.indices[k]);
        out.values.push_back(m.values[k]);
      }
    }
    out.indptr.push_back(out.cols.size());
  }
  return out;
}

/// What the similarity pass reads, built once from the training set.
struct Prepared {
  std::uint32_t n_items = 0;
  std::vector<std::uint8_t> eligible;  // item has >= min_support training ratings
  std::vector<double> user_mean;       // mu_u over ALL of u's training ratings
  double global_mean = 0.0;            // mean of every training rating
  Rows by_user;                        // users x items, eligible items only
  Rows by_item;                        // items x users, eligible items only (ineligible rows empty)
};

Prepared prepare(const Dataset& train, const Params& p) {
  const Csr& U = train.by_user;
  const Csr& I = train.by_item;
  if (U.n_rows != train.n_users() || U.n_cols != train.n_items() || I.n_rows != train.n_items() ||
      I.n_cols != train.n_users() || I.nnz() != U.nnz()) {
    throw std::invalid_argument("knn: dataset matrices are inconsistent with its id maps");
  }
  Prepared pr;
  pr.n_items = train.n_items();
  pr.eligible.resize(pr.n_items);
  for (std::uint32_t i = 0; i < pr.n_items; ++i) pr.eligible[i] = I.row_size(i) >= p.min_support ? 1 : 0;
  pr.user_mean.resize(U.n_rows);
  for (std::uint32_t u = 0; u < U.n_rows; ++u) pr.user_mean[u] = user_mean(U.row_values(u));
  pr.global_mean = user_mean(U.values);
  pr.by_user = filter_columns(U, [&](std::uint32_t item) { return pr.eligible[item] != 0; });
  // by_item: keep every column (user) but only the rows of eligible items.
  pr.by_item.indptr.reserve(static_cast<std::size_t>(pr.n_items) + 1);
  for (std::uint32_t i = 0; i < pr.n_items; ++i) {
    if (pr.eligible[i]) {
      const auto users = I.row_indices(i);
      const auto vals = I.row_values(i);
      pr.by_item.cols.insert(pr.by_item.cols.end(), users.begin(), users.end());
      pr.by_item.values.insert(pr.by_item.values.end(), vals.begin(), vals.end());
    }
    pr.by_item.indptr.push_back(pr.by_item.cols.size());
  }
  return pr;
}

/// Per-thread scratch for one similarity row: dense sums indexed by the partner item j.
struct Accumulator {
  std::vector<double> dot;      // sum c_ui c_uj
  std::vector<double> norm_i;   // sum c_ui^2   (over co-raters)
  std::vector<double> norm_j;   // sum c_uj^2   (over co-raters)
  std::vector<std::uint32_t> support;  // n_ij
  std::vector<ItemIdx> touched;        // partners with support > 0, to reset cheaply

  explicit Accumulator(std::uint32_t n_items)
      : dot(n_items, 0.0), norm_i(n_items, 0.0), norm_j(n_items, 0.0), support(n_items, 0) {}
};

/// The full (untruncated) similarity row of item i, sorted by partner ItemIdx. Empty if i is not
/// eligible. This is the single place the similarity formula lives; fit() and the test hook
/// full_similarity() both call it.
void similarity_row(const Prepared& pr, const Params& p, ItemIdx i, Accumulator& acc,
                    std::vector<PairSimilarity>& out) {
  out.clear();
  if (!pr.eligible[i]) return;
  for (std::uint64_t a = pr.by_item.begin(i); a < pr.by_item.end(i); ++a) {
    const std::uint32_t u = pr.by_item.cols[a];
    const double mu = pr.user_mean[u];
    const double ci = static_cast<double>(pr.by_item.values[a]) - mu;
    const double ci2 = ci * ci;
    for (std::uint64_t b = pr.by_user.begin(u); b < pr.by_user.end(u); ++b) {
      const ItemIdx j = pr.by_user.cols[b];
      if (j == i) continue;
      const double cj = static_cast<double>(pr.by_user.values[b]) - mu;
      if (acc.support[j] == 0) acc.touched.push_back(j);
      acc.support[j] += 1;
      acc.dot[j] += ci * cj;
      acc.norm_i[j] += ci2;
      acc.norm_j[j] += cj * cj;
    }
  }
  std::sort(acc.touched.begin(), acc.touched.end());
  out.reserve(acc.touched.size());
  for (ItemIdx j : acc.touched) {
    const double denom = std::sqrt(acc.norm_i[j]) * std::sqrt(acc.norm_j[j]);
    double s = 0.0;
    if (denom > 0.0) s = std::clamp(acc.dot[j] / denom, -1.0, 1.0);
    const double n = static_cast<double>(acc.support[j]);
    const double shrunk = s * (n / (n + p.shrinkage));
    out.push_back(PairSimilarity{j, acc.support[j], s, shrunk});
    acc.dot[j] = acc.norm_i[j] = acc.norm_j[j] = 0.0;
    acc.support[j] = 0;
  }
  acc.touched.clear();
}

/// Strict total order for neighbour selection: |s'| descending, then ItemIdx ascending.
bool stronger(const PairSimilarity& a, const PairSimilarity& b) {
  const double x = std::abs(a.shrunk), y = std::abs(b.shrunk);
  if (x != y) return x > y;
  return a.item < b.item;
}

// ---------------------------------------------------------------------------------------------
// Little-endian binary I/O
// ---------------------------------------------------------------------------------------------
constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'K', 'N', 'N', '1'};
constexpr std::uint32_t kFormatVersion = 3;  // 2 added normalize and positive_only; 3 mean_damping
constexpr const char* kFileName = "item_knn.bin";

class Writer {
 public:
  void bytes(const void* p, std::size_t n) { buf_.append(static_cast<const char*>(p), n); }
  void u8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) { for (int k = 0; k < 4; ++k) buf_.push_back(static_cast<char>((v >> (8 * k)) & 0xFF)); }
  void u64(std::uint64_t v) { for (int k = 0; k < 8; ++k) buf_.push_back(static_cast<char>((v >> (8 * k)) & 0xFF)); }
  void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
  void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
  const std::string& data() const { return buf_; }

 private:
  std::string buf_;
};

class Reader {
 public:
  explicit Reader(std::string data) : buf_(std::move(data)) {}
  void bytes(void* p, std::size_t n) { need(n); std::memcpy(p, buf_.data() + pos_, n); pos_ += n; }
  std::uint8_t u8() {
    need(1);
    return static_cast<std::uint8_t>(buf_[pos_++]);
  }
  std::uint32_t u32() {
    need(4);
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(buf_[pos_++])) << (8 * k);
    return v;
  }
  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf_[pos_++])) << (8 * k);
    return v;
  }
  float f32() { return std::bit_cast<float>(u32()); }
  double f64() { return std::bit_cast<double>(u64()); }
  std::size_t remaining() const { return buf_.size() - pos_; }

 private:
  void need(std::size_t n) const {
    if (buf_.size() - pos_ < n) throw std::runtime_error("knn: model file is truncated");
  }
  std::string buf_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------------------------
class ItemKnn final : public Model {
 public:
  std::string_view name() const override { return "knn"; }

  void fit(const Dataset& train, const Config& cfg, const TrainContext& ctx) override {
    // No randomness anywhere, so ctx.seed is unused; the parallel loop is deterministic for every
    // thread count (see the header comment), and uses a static schedule as the contract requires.
    (void)ctx;
    params_ = item_knn_detail::parse_config(cfg);
    Prepared pr = prepare(train, params_);
    n_items_ = pr.n_items;

    std::vector<std::vector<PairSimilarity>> lists(n_items_);
    const auto n = static_cast<std::int64_t>(n_items_);
#pragma omp parallel
    {
      Accumulator acc(n_items_);
      std::vector<PairSimilarity> row;
      // Small static chunks interleave cheap and expensive items across threads.
#pragma omp for schedule(static, 16)
      for (std::int64_t ii = 0; ii < n; ++ii) {
        const auto i = static_cast<ItemIdx>(ii);
        similarity_row(pr, params_, i, acc, row);
        const bool positive_only = params_.positive_only;
        std::erase_if(row, [&](const PairSimilarity& p) { return positive_only ? p.shrunk <= 0.0 : p.shrunk == 0.0; });
        const std::size_t keep = std::min<std::size_t>(row.size(), params_.neighbors);
        std::partial_sort(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(keep), row.end(), stronger);
        row.resize(keep);
        lists[i] = row;
      }
    }

    nbr_ptr_.assign(1, 0);
    nbr_item_.clear();
    nbr_sim_.clear();
    for (const auto& l : lists) {
      for (const auto& p : l) {
        nbr_item_.push_back(p.item);
        nbr_sim_.push_back(static_cast<float>(p.shrunk));
      }
      nbr_ptr_.push_back(nbr_item_.size());
    }

    // Keep what the batch path needs: each training user's scoring mean and eligible ratings.
    // Similarities above use the plain mean; scoring uses the damped one (equal when beta = 0).
    global_mean_ = pr.global_mean;
    if (params_.mean_damping != 0.0) {
      for (std::uint32_t u = 0; u < train.by_user.n_rows; ++u) {
        pr.user_mean[u] = damped_mean(train.by_user.row_values(u), params_.mean_damping, global_mean_);
      }
    }
    train_mean_ = std::move(pr.user_mean);
    train_rows_ = std::move(pr.by_user);
    has_training_ = true;
  }

  UserState fold_in(std::span<const Rating> ratings) const override {
    UserState st;
    st.ratings.assign(ratings.begin(), ratings.end());
    std::stable_sort(st.ratings.begin(), st.ratings.end(),
                     [](const Rating& a, const Rating& b) { return a.item < b.item; });
    std::vector<float> values;
    values.reserve(st.ratings.size());
    for (std::size_t k = 0; k < st.ratings.size(); ++k) {
      const Rating& r = st.ratings[k];
      if (r.item >= n_items_) throw std::invalid_argument("knn: fold_in rating for an unknown item");
      if (k > 0 && st.ratings[k - 1].item == r.item) throw std::invalid_argument("knn: fold_in got the same item twice");
      if (!std::isfinite(r.value)) throw std::invalid_argument("knn: fold_in rating is not finite");
      values.push_back(r.value);
    }
    st.mean = damped_mean(values, params_.mean_damping, global_mean_);
    return st;
  }

  void score(const UserState& u, std::span<float> out) const override {
    // A UserState may be built by hand rather than by fold_in(), so check what fold_in() would
    // have guaranteed: a NaN or infinite input would otherwise turn into NaN scores.
    if (!std::isfinite(u.mean)) throw std::invalid_argument("knn: user state mean is not finite");
    std::vector<double> dev(n_items_, 0.0);
    std::vector<std::uint8_t> rated(n_items_, 0);
    for (const Rating& r : u.ratings) {
      if (r.item >= n_items_) throw std::invalid_argument("knn: user state rates an unknown item");
      if (!std::isfinite(r.value)) throw std::invalid_argument("knn: user state rating is not finite");
      dev[r.item] = static_cast<double>(r.value) - u.mean;
      rated[r.item] = 1;
    }
    score_dense(dev, rated, out);
  }

  bool supports_trained_scoring() const override { return has_training_; }

  void score_trained_user(UserIdx u, std::span<float> out) const override {
    if (!has_training_) throw NotImplemented("knn: a loaded model keeps no training rows");
    if (u >= train_mean_.size()) throw std::out_of_range("knn: training user out of range");
    std::vector<double> dev(n_items_, 0.0);
    std::vector<std::uint8_t> rated(n_items_, 0);
    const double mu = train_mean_[u];
    for (std::uint64_t k = train_rows_.begin(u); k < train_rows_.end(u); ++k) {
      dev[train_rows_.cols[k]] = static_cast<double>(train_rows_.values[k]) - mu;
      rated[train_rows_.cols[k]] = 1;
    }
    score_dense(dev, rated, out);
  }

  Explanation explain(const UserState& u, ItemIdx item) const override {
    if (item >= n_items_) throw std::out_of_range("knn: explain item out of range");
    if (!std::isfinite(u.mean)) throw std::invalid_argument("knn: user state mean is not finite");
    std::vector<Rating> sorted = u.ratings;
    std::sort(sorted.begin(), sorted.end(), [](const Rating& a, const Rating& b) { return a.item < b.item; });
    auto find = [&](ItemIdx j) -> const Rating* {
      auto it = std::lower_bound(sorted.begin(), sorted.end(), j,
                                 [](const Rating& r, ItemIdx x) { return r.item < x; });
      return (it != sorted.end() && it->item == j) ? &*it : nullptr;
    };
    // Same sums as score(): each rated neighbour contributes s' * (r - mu), divided by sum |s'|
    // when normalize is on.
    std::vector<std::pair<ItemIdx, double>> terms;
    double den = 0.0;
    for (std::uint64_t k = nbr_ptr_[item]; k < nbr_ptr_[item + 1]; ++k) {
      const Rating* r = find(nbr_item_[k]);
      if (r == nullptr) continue;
      const double s = static_cast<double>(nbr_sim_[k]);
      terms.emplace_back(nbr_item_[k], s * (static_cast<double>(r->value) - u.mean));
      den += std::abs(s);
    }
    Explanation e;
    if (den <= 0.0) return e;
    for (const auto& [j, t] : terms) {
      const auto w = static_cast<float>(params_.normalize ? t / den : t);
      if (w > 0.0f) e.because.push_back(Because{j, w});
    }
    std::sort(e.because.begin(), e.because.end(), [](const Because& a, const Because& b) {
      if (a.weight != b.weight) return a.weight > b.weight;
      return a.item < b.item;
    });
    if (e.because.size() > 3) e.because.resize(3);
    return e;
  }

  bool supports_similar() const override { return true; }

  /// The most similar items: the positive part of the neighbour list, strongest first.
  std::vector<Scored> similar(ItemIdx item, std::size_t k) const override {
    if (item >= n_items_) throw std::out_of_range("knn: similar item out of range");
    std::vector<Scored> out;
    for (std::uint64_t e = nbr_ptr_[item]; e < nbr_ptr_[item + 1] && out.size() < k; ++e) {
      if (nbr_sim_[e] > 0.0f) out.push_back(Scored{nbr_item_[e], nbr_sim_[e]});
    }
    return out;
  }

  std::vector<Scored> neighbours(ItemIdx item) const {
    if (item >= n_items_) throw std::out_of_range("knn: item out of range");
    std::vector<Scored> out;
    for (std::uint64_t e = nbr_ptr_[item]; e < nbr_ptr_[item + 1]; ++e) out.push_back(Scored{nbr_item_[e], nbr_sim_[e]});
    return out;
  }

  // File layout (all little-endian):
  //   char[8]  magic "RECSKNN1"
  //   u32      format version (kFormatVersion, currently 2)
  //   u32      n_items
  //   u32      neighbors      u32 min_support      f64 shrinkage
  //   u8       normalize (0/1)  u8 positive_only (0/1)
  //   f64      mean_damping     f64 global_mean      (format 3; a format 2 file reads as 0, 0)
  //   u64      nnz (total neighbour entries)
  //   u64[n_items + 1]  row offsets
  //   u32[nnz]          neighbour ItemIdx
  //   f32[nnz]          shrunk similarity s'
  void save(const fs::path& dir) const override {
    Writer w;
    w.bytes(kMagic, sizeof kMagic);
    w.u32(kFormatVersion);
    w.u32(n_items_);
    w.u32(params_.neighbors);
    w.u32(params_.min_support);
    w.f64(params_.shrinkage);
    w.u8(params_.normalize ? 1 : 0);
    w.u8(params_.positive_only ? 1 : 0);
    w.f64(params_.mean_damping);
    w.f64(global_mean_);
    w.u64(nbr_item_.size());
    for (auto v : nbr_ptr_) w.u64(v);
    for (auto v : nbr_item_) w.u32(v);
    for (auto v : nbr_sim_) w.f32(v);
    const fs::path file = dir / kFileName;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("knn: cannot open " + file.string() + " for writing");
    out.write(w.data().data(), static_cast<std::streamsize>(w.data().size()));
    out.close();
    if (!out) throw std::runtime_error("knn: failed writing " + file.string());
  }

  void load(const fs::path& dir) override {
    const fs::path file = dir / kFileName;
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("knn: cannot open " + file.string());
    Reader r(std::string(std::istreambuf_iterator<char>(in), {}));

    char magic[8];
    r.bytes(magic, sizeof magic);
    if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) throw std::runtime_error("knn: bad magic in " + file.string());
    const std::uint32_t version = r.u32();
    if (version != 2 && version != kFormatVersion) throw std::runtime_error("knn: unsupported format version");
    const std::uint32_t n_items = r.u32();
    Params p;
    p.neighbors = r.u32();
    p.min_support = r.u32();
    p.shrinkage = r.f64();
    const std::uint8_t normalize = r.u8();
    const std::uint8_t positive_only = r.u8();
    if (normalize > 1 || positive_only > 1) throw std::runtime_error("knn: bad flag byte");
    p.normalize = normalize == 1;
    p.positive_only = positive_only == 1;
    // A format 2 file predates mean_damping: it was trained and scored with the plain mean.
    p.mean_damping = 0.0;
    double global_mean = 0.0;
    if (version >= 3) {
      p.mean_damping = r.f64();
      global_mean = r.f64();
      if (!std::isfinite(global_mean)) throw std::runtime_error("knn: bad global mean");
    }
    item_knn_detail::parse_config(item_knn_detail::to_config(p));  // same validation as fit()
    const std::uint64_t nnz = r.u64();
    // Size check before allocating, so a corrupt count cannot trigger a huge allocation.
    // nnz is bounded by the file size first, so nnz * 8 below cannot overflow.
    if (nnz > r.remaining()) throw std::runtime_error("knn: model file has the wrong size");
    const std::uint64_t expect = (static_cast<std::uint64_t>(n_items) + 1) * 8 + nnz * 8;
    if (expect != r.remaining()) throw std::runtime_error("knn: model file has the wrong size");

    std::vector<std::uint64_t> ptr(static_cast<std::size_t>(n_items) + 1);
    for (auto& v : ptr) v = r.u64();
    std::vector<ItemIdx> items(nnz);
    for (auto& v : items) v = r.u32();
    std::vector<float> sims(nnz);
    for (auto& v : sims) v = r.f32();

    // Validate the whole offset array before walking any row (audit SA2-1).
    if (const auto err = recs::detail::check_row_offsets(ptr, n_items, nnz)) throw std::runtime_error("knn: bad " + *err);
    // seen_in_row[j] == i + 1 marks j as already listed in row i (catches duplicate ids).
    std::vector<std::uint32_t> seen_in_row(n_items, 0);
    for (std::uint32_t i = 0; i < n_items; ++i) {
      if (ptr[i + 1] - ptr[i] > p.neighbors) throw std::runtime_error("knn: bad row offsets");
      for (std::uint64_t e = ptr[i]; e < ptr[i + 1]; ++e) {
        if (items[e] >= n_items || items[e] == i) throw std::runtime_error("knn: bad neighbour id");
        if (seen_in_row[items[e]] == i + 1) throw std::runtime_error("knn: duplicate neighbour id");
        seen_in_row[items[e]] = i + 1;
        if (!std::isfinite(sims[e]) || sims[e] == 0.0f || std::abs(sims[e]) > 1.0f ||
            (p.positive_only && sims[e] < 0.0f)) {
          throw std::runtime_error("knn: bad similarity value");
        }
        if (e > ptr[i] && std::abs(sims[e]) > std::abs(sims[e - 1])) throw std::runtime_error("knn: neighbour list out of order");
      }
    }

    params_ = p;
    global_mean_ = global_mean;
    n_items_ = n_items;
    nbr_ptr_ = std::move(ptr);
    nbr_item_ = std::move(items);
    nbr_sim_ = std::move(sims);
    train_mean_.clear();
    train_rows_ = Rows{};
    has_training_ = false;
  }

  std::uint32_t n_items() const override { return n_items_; }
  Config config() const override { return item_knn_detail::to_config(params_); }

 private:
  /// The scoring formula, shared by score() and score_trained_user(). dev[j] = r_uj - mu_u for
  /// rated items (rated[j] = 1). Neighbours are visited in stored order, so the sums are added in
  /// the same order on every path.
  void score_dense(const std::vector<double>& dev, const std::vector<std::uint8_t>& rated, std::span<float> out) const {
    if (out.size() != n_items_) throw std::invalid_argument("knn: score output has the wrong size");
    for (std::uint32_t i = 0; i < n_items_; ++i) {
      double num = 0.0, den = 0.0;
      for (std::uint64_t e = nbr_ptr_[i]; e < nbr_ptr_[i + 1]; ++e) {
        const ItemIdx j = nbr_item_[e];
        if (!rated[j]) continue;
        const double s = static_cast<double>(nbr_sim_[e]);
        num += s * dev[j];
        den += std::abs(s);
      }
      if (den <= 0.0) {
        out[i] = kCannotScore;  // no rated neighbour (either mode)
      } else {
        out[i] = static_cast<float>(params_.normalize ? num / den : num);
      }
    }
  }

  Params params_;
  std::uint32_t n_items_ = 0;
  // Neighbour lists in CSR form: item i's list is [nbr_ptr_[i], nbr_ptr_[i+1]).
  std::vector<std::uint64_t> nbr_ptr_{0};
  std::vector<ItemIdx> nbr_item_;
  std::vector<float> nbr_sim_;
  // Batch path (present after fit(), absent after load()).
  double global_mean_ = 0.0;  // g in the damped scoring mean
  bool has_training_ = false;
  std::vector<double> train_mean_;  // each training user's scoring (damped) mean
  Rows train_rows_;
};

}  // namespace

std::unique_ptr<Model> make_item_knn() { return std::make_unique<ItemKnn>(); }

namespace item_knn_detail {

std::vector<std::vector<PairSimilarity>> full_similarity(const Dataset& train, const Config& cfg) {
  const Params p = parse_config(cfg);
  const Prepared pr = prepare(train, p);
  std::vector<std::vector<PairSimilarity>> rows(pr.n_items);
  Accumulator acc(pr.n_items);
  for (ItemIdx i = 0; i < pr.n_items; ++i) similarity_row(pr, p, i, acc, rows[i]);
  return rows;
}

std::vector<Scored> neighbour_list(const Model& m, ItemIdx item) {
  const auto* knn = dynamic_cast<const ItemKnn*>(&m);
  if (knn == nullptr) throw std::invalid_argument("neighbour_list: not an item-KNN model");
  return knn->neighbours(item);
}

}  // namespace item_knn_detail
}  // namespace recs
