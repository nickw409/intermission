// Implicit-feedback matrix factorisation by alternating least squares ("als").
//
// ---------------------------------------------------------------------------------------------
// The model (Hu, Koren & Volinsky 2008)
// ---------------------------------------------------------------------------------------------
// Ratings are turned into implicit feedback: a rating r_ui >= interaction_threshold (3.5) is an
// "interaction". For EVERY (user, item) pair, interacted or not, define
//   preference  p_ui = 1 if (u, i) is an interaction, else 0
//   confidence  c_ui = 1 + alpha if (u, i) is an interaction, else 1
// (a rating below the threshold is treated exactly like a missing one). With user factors x_u and
// item factors y_i in R^k, ALS minimises over all n_users * n_items pairs
//
//   L(X, Y) = sum_{u,i} c_ui (p_ui - x_u . y_i)^2  +  lambda ( sum_u |x_u|^2 + sum_i |y_i|^2 ).
//
// With Y fixed, L is a ridge regression in each x_u separately, with the closed form
//
//   x_u = (Y^T C^u Y + lambda I)^{-1} Y^T C^u p_u,          C^u = diag(c_u1, ..., c_un)
//
// and symmetrically for y_i with X fixed. Alternating the two "half-steps" never increases L
// (each one is an exact block minimisation), which is the first Phase 4 gate.
//
// The Y^T Y trick. Y^T C^u Y sums over every item, but C^u - I is zero except on the user's
// interactions I_u, so
//
//   Y^T C^u Y   = Y^T Y + alpha * sum_{i in I_u} y_i y_i^T
//   Y^T C^u p_u = (1 + alpha) * sum_{i in I_u} y_i.
//
// G = Y^T Y is computed once per half-step; each user then costs O(|I_u| k^2 + k^3) instead of
// O(n_items k^2). The k x k system is solved by Cholesky (LLT) in double precision (solve_spd);
// the factors are stored as float32.
//
// The objective uses the same trick (all sums in double):
//   sum_{u,i} c_ui (p_ui - s_ui)^2 = sum_{u,i} s_ui^2 + sum_{(u,i) interactions} [(1+alpha)(1-s_ui)^2 - s_ui^2]
// with sum_{u,i} s_ui^2 = trace(X^T X Y^T Y) = sum_ab (X^T X)_ab (Y^T Y)_ab.
//
// Training order. X ~ N(0, init_sd^2); each iteration solves the items (given X), then the users
// (given Y). Ending on the user half-step means the stored user vectors are the exact solves
// against the final Y, i.e. precisely what fold_in() computes for the same ratings: fold-in
// "is exactly this solve with Y fixed" (spec), so the batch path and fold-in agree bit for bit.
// training_curve() records L after each full iteration ("objective").
//
// Scoring is x_u . y_i. Items with no interaction in training have y_i = 0 and nothing to say,
// so they score -infinity ("cannot score") and are never recommended. A user with no
// interactions folds in to x_u = 0 (all warm items score 0); callers fall back to popularity.
//
// Explanations (Hu et al. 2008, section 5). With W^u = (Y^T C^u Y + lambda I)^{-1},
//   score_ui = y_i . x_u = sum_{j in I_u} (1 + alpha) y_i^T W^u y_j,
// so each interacted item j contributes (1 + alpha) y_i^T W^u y_j. explain() returns the (at most
// three) largest positive contributions; only the user's own interactions can appear.
//
// Determinism. Every row's solve depends only on that row's inputs and on G, and G is summed in
// fixed-size row blocks that are then added in block order. Rows are distributed with a static
// OpenMP schedule and no shared accumulators, so the result is bit-identical for a given seed
// regardless of the number of threads (ctx.deterministic is therefore always satisfied).
//
// References
//   Y. Hu, Y. Koren, C. Volinsky. Collaborative Filtering for Implicit Feedback Datasets.
//     ICDM 2008.
//   G. Takacs, I. Pilaszy, D. Tikk. Applications of the conjugate gradient method for implicit
//     feedback collaborative filtering. RecSys 2011 (for the cost analysis of the exact solve).
//   S. Rendle, W. Krichene, L. Zhang, Y. Koren. Revisiting the Performance of iALS on Item
//     Recommendation Benchmarks. RecSys 2022.
// ---------------------------------------------------------------------------------------------
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <Eigen/Core>

#include "models/als_detail.hpp"
#include "recs/foldin.hpp"
#include "recs/model.hpp"
#include "recs/rng.hpp"
#include "recs/sparse.hpp"

namespace recs {
namespace als_detail {

namespace {

// Rows per block in gram() and per batch in build_system(). Fixed, so sums do not depend on the
// thread count.
constexpr std::size_t kBlockRows = 1024;

using RowMajorMatrix = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

// Copies rows `rows[first, first + m)` of the float matrix F into the first m rows of B (double).
void gather_rows(std::span<const float> F, std::size_t k, std::span<const std::uint32_t> rows,
                 std::size_t first, std::size_t m, RowMajorMatrix& B) {
  for (std::size_t r = 0; r < m; ++r) {
    const float* src = F.data() + static_cast<std::size_t>(rows[first + r]) * k;
    for (std::size_t a = 0; a < k; ++a) B(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(a)) = src[a];
  }
}

// Runs body(r) for r in [0, n) with a static OpenMP schedule. An exception thrown by any row is
// rethrown on the calling thread after the loop (exceptions must not escape an OpenMP region).
template <class Body>
void parallel_rows(std::size_t n, Body&& body) {
  std::exception_ptr error;
  std::mutex error_mutex;
  const auto count = static_cast<std::int64_t>(n);
#pragma omp parallel for schedule(static, 16)
  for (std::int64_t r = 0; r < count; ++r) {
    try {
      body(static_cast<std::size_t>(r));
    } catch (...) {
      const std::lock_guard<std::mutex> lock(error_mutex);
      if (!error) error = std::current_exception();
    }
  }
  if (error) std::rethrow_exception(error);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Config

Params parse_config(const Config& cfg) {
  if (!cfg.is_object()) throw std::invalid_argument("als config must be a JSON object");
  static const std::set<std::string> known{"k", "lambda", "alpha", "iters", "interaction_threshold", "init_sd"};
  for (const auto& [key, value] : cfg.items())
    if (known.count(key) == 0) throw std::invalid_argument("als config: unknown key '" + key + "'");

  const auto number = [&](const char* key, double fallback) {
    if (!cfg.contains(key)) return fallback;
    const auto& v = cfg.at(key);
    if (!v.is_number()) throw std::invalid_argument(std::string("als config: '") + key + "' must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d)) throw std::invalid_argument(std::string("als config: '") + key + "' must be finite");
    return d;
  };
  const auto integer = [&](const char* key, std::int64_t fallback, std::int64_t lo, std::int64_t hi) {
    if (!cfg.contains(key)) return fallback;
    const auto& v = cfg.at(key);
    if (!v.is_number_integer()) throw std::invalid_argument(std::string("als config: '") + key + "' must be an integer");
    const auto i = v.get<std::int64_t>();
    if (i < lo || i > hi)
      throw std::invalid_argument(std::string("als config: '") + key + "' must be in [" + std::to_string(lo) + ", " +
                                  std::to_string(hi) + "]");
    return i;
  };

  const Params d;
  Params p;
  p.k = static_cast<std::uint32_t>(integer("k", d.k, 1, 1024));
  p.iters = static_cast<int>(integer("iters", d.iters, 0, 1000));
  p.lambda = number("lambda", d.lambda);
  p.alpha = number("alpha", d.alpha);
  p.interaction_threshold = static_cast<float>(number("interaction_threshold", d.interaction_threshold));
  p.init_sd = number("init_sd", d.init_sd);
  if (!(p.lambda > 0.0)) throw std::invalid_argument("als config: 'lambda' must be > 0");
  if (!(p.alpha >= 0.0)) throw std::invalid_argument("als config: 'alpha' must be >= 0");
  if (!(p.init_sd > 0.0)) throw std::invalid_argument("als config: 'init_sd' must be > 0");
  if (!(p.interaction_threshold > 0.0f && p.interaction_threshold <= 5.0f))
    throw std::invalid_argument("als config: 'interaction_threshold' must be in (0, 5]");
  return p;
}

Config to_config(const Params& p) {
  return Config{{"k", p.k},
                {"lambda", p.lambda},
                {"alpha", p.alpha},
                {"iters", p.iters},
                {"interaction_threshold", static_cast<double>(p.interaction_threshold)},
                {"init_sd", p.init_sd}};
}

// ---------------------------------------------------------------------------------------------
// Interactions

Interactions build_interactions(const Dataset& train, float threshold) {
  const Csr& R = train.by_user;
  Interactions out;
  Csr& U = out.by_user;
  U.n_rows = R.n_rows;
  U.n_cols = R.n_cols;
  U.indptr.assign(1, 0);
  U.indptr.reserve(static_cast<std::size_t>(R.n_rows) + 1);
  for (std::uint32_t u = 0; u < R.n_rows; ++u) {
    const auto items = R.row_indices(u);
    const auto values = R.row_values(u);
    for (std::size_t j = 0; j < items.size(); ++j)
      if (values[j] >= threshold) U.indices.push_back(items[j]);
    U.indptr.push_back(U.indices.size());
  }
  U.values.assign(U.indices.size(), 1.0f);
  U.ts.assign(U.indices.size(), 0);
  out.by_item = transpose(U);
  return out;
}

std::vector<std::uint32_t> interacted_items(std::span<const Rating> ratings, float threshold, std::uint32_t n_items) {
  std::vector<std::uint32_t> items;
  items.reserve(ratings.size());
  for (const auto& r : ratings) {
    if (r.item >= n_items) throw std::invalid_argument("als: rating for unknown item " + std::to_string(r.item));
    if (r.value >= threshold) items.push_back(r.item);
  }
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
  return items;
}

// ---------------------------------------------------------------------------------------------
// Linear algebra

std::vector<double> gram(std::span<const float> F, std::size_t n_rows, std::size_t k) {
  if (F.size() != n_rows * k) throw std::invalid_argument("als gram: size mismatch");
  const std::size_t n_blocks = (n_rows + kBlockRows - 1) / kBlockRows;
  const auto K = static_cast<Eigen::Index>(k);
  std::vector<Eigen::MatrixXd> partial(n_blocks);
  parallel_rows(n_blocks, [&](std::size_t b) {
    const std::size_t first = b * kBlockRows;
    const std::size_t m = std::min(kBlockRows, n_rows - first);
    RowMajorMatrix B(static_cast<Eigen::Index>(m), K);
    for (std::size_t r = 0; r < m; ++r)
      for (std::size_t a = 0; a < k; ++a)
        B(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(a)) = F[(first + r) * k + a];
    partial[b] = Eigen::MatrixXd::Zero(K, K);
    partial[b].selfadjointView<Eigen::Lower>().rankUpdate(B.transpose());
  });
  Eigen::MatrixXd G = Eigen::MatrixXd::Zero(K, K);
  for (const auto& P : partial) G += P;  // block order: independent of the thread count
  std::vector<double> out(k * k);
  for (std::size_t a = 0; a < k; ++a)
    for (std::size_t c = 0; c <= a; ++c)
      out[a * k + c] = out[c * k + a] = G(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c));
  return out;
}

System build_system(std::span<const double> G, std::span<const float> F, std::size_t k,
                    std::span<const std::uint32_t> rows, double alpha) {
  if (G.size() != k * k) throw std::invalid_argument("als build_system: G size mismatch");
  const auto K = static_cast<Eigen::Index>(k);
  Eigen::MatrixXd A = Eigen::Map<const RowMajorMatrix>(G.data(), K, K);
  Eigen::VectorXd sum = Eigen::VectorXd::Zero(K);
  if (!rows.empty()) {
    RowMajorMatrix B(static_cast<Eigen::Index>(std::min(kBlockRows, rows.size())), K);
    for (std::size_t first = 0; first < rows.size(); first += kBlockRows) {
      const std::size_t m = std::min(kBlockRows, rows.size() - first);
      gather_rows(F, k, rows, first, m, B);
      const auto block = B.topRows(static_cast<Eigen::Index>(m));
      if (alpha != 0.0) A.selfadjointView<Eigen::Lower>().rankUpdate(block.transpose(), alpha);
      sum += block.colwise().sum().transpose();
    }
  }
  System s;
  s.A.resize(k * k);
  for (std::size_t a = 0; a < k; ++a)
    for (std::size_t c = 0; c <= a; ++c)
      s.A[a * k + c] = s.A[c * k + a] = A(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c));
  s.b.resize(k);
  for (std::size_t a = 0; a < k; ++a) s.b[a] = (1.0 + alpha) * sum(static_cast<Eigen::Index>(a));
  return s;
}

std::vector<double> solve_row(std::span<const double> G, std::span<const float> F, std::size_t k,
                              std::span<const std::uint32_t> rows, double alpha, double lambda) {
  if (rows.empty()) return std::vector<double>(k, 0.0);  // b = 0, so the exact solution is 0
  const System s = build_system(G, F, k, rows, alpha);
  return solve_spd(s.A, s.b, k, lambda);
}

void half_step(const Csr& pattern, std::span<const float> F, std::size_t k, double alpha, double lambda,
               std::vector<float>& target) {
  if (F.size() != static_cast<std::size_t>(pattern.n_cols) * k) throw std::invalid_argument("als half_step: F size mismatch");
  const std::vector<double> G = gram(F, pattern.n_cols, k);
  target.resize(static_cast<std::size_t>(pattern.n_rows) * k);
  parallel_rows(pattern.n_rows, [&](std::size_t r) {
    const auto x = solve_row(G, F, k, pattern.row_indices(static_cast<std::uint32_t>(r)), alpha, lambda);
    for (std::size_t a = 0; a < k; ++a) target[r * k + a] = static_cast<float>(x[a]);
  });
}

double objective(const Interactions& data, std::span<const float> X, std::span<const float> Y, std::size_t k,
                 double alpha, double lambda) {
  const std::uint32_t n_users = data.by_user.n_rows;
  const std::uint32_t n_items = data.by_user.n_cols;
  const auto GX = gram(X, n_users, k);
  const auto GY = gram(Y, n_items, k);
  double all_pairs = 0.0;  // sum over every (u, i) of s_ui^2
  double reg = 0.0;
  for (std::size_t a = 0; a < k * k; ++a) all_pairs += GX[a] * GY[a];
  for (std::size_t a = 0; a < k; ++a) reg += GX[a * k + a] + GY[a * k + a];

  std::vector<double> per_user(n_users, 0.0);
  parallel_rows(n_users, [&](std::size_t u) {
    double acc = 0.0;
    for (std::uint32_t i : data.by_user.row_indices(static_cast<std::uint32_t>(u))) {
      double s = 0.0;
      for (std::size_t a = 0; a < k; ++a) s += static_cast<double>(X[u * k + a]) * static_cast<double>(Y[i * k + a]);
      acc += (1.0 + alpha) * (1.0 - s) * (1.0 - s) - s * s;
    }
    per_user[u] = acc;
  });
  double interactions = 0.0;
  for (double v : per_user) interactions += v;  // user order: independent of the thread count
  return all_pairs + interactions + lambda * reg;
}

std::vector<double> contributions(std::span<const double> G, std::span<const float> F, std::size_t k,
                                  std::span<const std::uint32_t> rows, double alpha, double lambda,
                                  std::uint32_t item) {
  if ((static_cast<std::size_t>(item) + 1) * k > F.size()) throw std::invalid_argument("als contributions: item out of range");
  std::vector<double> out(rows.size(), 0.0);
  if (rows.empty()) return out;
  const System s = build_system(G, F, k, rows, alpha);
  std::vector<double> y(k);
  for (std::size_t a = 0; a < k; ++a) y[a] = F[static_cast<std::size_t>(item) * k + a];
  const auto z = solve_spd(s.A, y, k, lambda);  // z = W y_item (W is symmetric)
  for (std::size_t j = 0; j < rows.size(); ++j) {
    double d = 0.0;
    for (std::size_t a = 0; a < k; ++a) d += z[a] * F[static_cast<std::size_t>(rows[j]) * k + a];
    out[j] = (1.0 + alpha) * d;
  }
  return out;
}

}  // namespace als_detail

// ---------------------------------------------------------------------------------------------
// The Model

namespace {

namespace ad = als_detail;

static_assert(std::endian::native == std::endian::little, "factor files are written in host byte order");

constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'A', 'L', 'S', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderBytes = sizeof(kMagic) + 3 * sizeof(std::uint32_t);
constexpr std::uint32_t kMaxItems = 50'000'000;

template <class T>
void put(std::string& buf, const T& v) {
  const auto* p = reinterpret_cast<const char*>(&v);
  buf.append(p, sizeof(T));
}

template <class T>
T get(const std::string& buf, std::size_t& pos) {
  T v;
  std::memcpy(&v, buf.data() + pos, sizeof(T));
  pos += sizeof(T);
  return v;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("als: cannot open " + p.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) throw std::runtime_error("als: cannot write " + p.string());
}

class AlsModel final : public Model {
 public:
  std::string_view name() const override { return "als"; }

  void fit(const Dataset& train, const Config& cfg, const TrainContext& ctx) override {
    const ad::Params p = ad::parse_config(cfg);
    const ad::Interactions data = ad::build_interactions(train, p.interaction_threshold);
    const std::size_t k = p.k;
    const std::uint32_t n_users = train.n_users();
    const std::uint32_t n_items = train.n_items();

    Rng rng(mix_seed(ctx.seed, 0x616C73));  // "als"
    std::vector<float> X(static_cast<std::size_t>(n_users) * k);
    std::vector<float> Y(static_cast<std::size_t>(n_items) * k);
    for (auto& x : X) x = static_cast<float>(p.init_sd * rng.normal());
    for (auto& y : Y) y = static_cast<float>(p.init_sd * rng.normal());

    std::vector<std::pair<int, double>> curve;
    for (int it = 1; it <= p.iters; ++it) {
      ad::half_step(data.by_item, X, k, p.alpha, p.lambda, Y);  // items given users
      ad::half_step(data.by_user, Y, k, p.alpha, p.lambda, X);  // users given items
      curve.emplace_back(it, ad::objective(data, X, Y, k, p.alpha, p.lambda));
    }

    std::vector<std::uint8_t> has_data(n_items);
    for (std::uint32_t i = 0; i < n_items; ++i) has_data[i] = data.by_item.row_size(i) > 0 ? 1 : 0;

    params_ = p;
    n_items_ = n_items;
    n_users_ = n_users;
    X_ = std::move(X);
    Y_ = std::move(Y);
    has_data_ = std::move(has_data);
    GY_ = ad::gram(Y_, n_items_, k);
    curve_ = std::move(curve);
    fitted_ = true;
  }

  UserState fold_in(std::span<const Rating> ratings) const override {
    require_fitted();
    const auto rows = ad::interacted_items(ratings, params_.interaction_threshold, n_items_);
    const auto x = ad::solve_row(GY_, Y_, params_.k, rows, params_.alpha, params_.lambda);
    UserState s;
    s.ratings.assign(ratings.begin(), ratings.end());
    s.vec.assign(x.begin(), x.end());  // double -> float32
    return s;
  }

  void score(const UserState& u, std::span<float> out) const override {
    require_fitted();
    if (u.vec.size() != params_.k) throw std::invalid_argument("als score: user vector has the wrong size");
    score_vector(u.vec.data(), out);
  }

  Explanation explain(const UserState& u, ItemIdx item) const override {
    require_fitted();
    if (item >= n_items_) throw std::invalid_argument("als explain: unknown item");
    Explanation e;
    if (!has_data_[item]) return e;
    const auto rows = ad::interacted_items(u.ratings, params_.interaction_threshold, n_items_);
    const auto c = ad::contributions(GY_, Y_, params_.k, rows, params_.alpha, params_.lambda, item);
    for (std::size_t j = 0; j < rows.size(); ++j)
      if (c[j] > 0.0) e.because.push_back({rows[j], static_cast<float>(c[j])});
    std::sort(e.because.begin(), e.because.end(), [](const Because& a, const Because& b) {
      return a.weight != b.weight ? a.weight > b.weight : a.item < b.item;
    });
    if (e.because.size() > 3) e.because.resize(3);
    return e;
  }

  // Files: config.json (the effective config) and factors.bin:
  //   magic "RECSALS1" | u32 format version | u32 n_items | u32 k |
  //   u8 has_data[n_items] | f32 Y[n_items * k] (row-major), all little-endian.
  // User factors are not saved (contracts/artifact.md rule 7); a loaded model serves fold-in only.
  void save(const fs::path& dir) const override {
    require_fitted();
    write_file(dir / "config.json", ad::to_config(params_).dump(2) + "\n");
    std::string buf;
    buf.reserve(kHeaderBytes + has_data_.size() + Y_.size() * sizeof(float));
    buf.append(kMagic, sizeof(kMagic));
    put(buf, kFormatVersion);
    put(buf, n_items_);
    put(buf, params_.k);
    buf.append(reinterpret_cast<const char*>(has_data_.data()), has_data_.size());
    buf.append(reinterpret_cast<const char*>(Y_.data()), Y_.size() * sizeof(float));
    write_file(dir / "factors.bin", buf);
  }

  void load(const fs::path& dir) override {
    const ad::Params p = ad::parse_config(Config::parse(read_file(dir / "config.json")));
    const std::string buf = read_file(dir / "factors.bin");
    if (buf.size() < kHeaderBytes || std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0)
      throw std::runtime_error("als: factors.bin has a bad header");
    std::size_t pos = sizeof(kMagic);
    const auto version = get<std::uint32_t>(buf, pos);
    const auto n_items = get<std::uint32_t>(buf, pos);
    const auto k = get<std::uint32_t>(buf, pos);
    if (version != kFormatVersion) throw std::runtime_error("als: unsupported factors.bin version");
    if (n_items > kMaxItems) throw std::runtime_error("als: factors.bin item count out of range");
    if (k != p.k) throw std::runtime_error("als: factors.bin k does not match config.json");
    const std::uint64_t n_values = static_cast<std::uint64_t>(n_items) * k;
    if (buf.size() != kHeaderBytes + n_items + n_values * sizeof(float))
      throw std::runtime_error("als: factors.bin has the wrong size");

    std::vector<std::uint8_t> has_data(buf.begin() + static_cast<std::ptrdiff_t>(pos),
                                       buf.begin() + static_cast<std::ptrdiff_t>(pos + n_items));
    pos += n_items;
    for (auto f : has_data)
      if (f > 1) throw std::runtime_error("als: factors.bin has an invalid item flag");
    std::vector<float> Y(n_values);
    std::memcpy(Y.data(), buf.data() + pos, n_values * sizeof(float));
    for (float y : Y)
      if (!std::isfinite(y)) throw std::runtime_error("als: factors.bin has a non-finite factor");

    params_ = p;
    n_items_ = n_items;
    n_users_ = 0;
    X_.clear();
    Y_ = std::move(Y);
    has_data_ = std::move(has_data);
    GY_ = ad::gram(Y_, n_items_, params_.k);
    curve_.clear();
    fitted_ = true;
  }

  std::uint32_t n_items() const override { return n_items_; }
  Config config() const override { return ad::to_config(params_); }

  bool supports_trained_scoring() const override { return fitted_ && !X_.empty(); }
  void score_trained_user(UserIdx u, std::span<float> out) const override {
    require_fitted();
    if (X_.empty()) throw NotImplemented("als: user factors are not available after load()");
    if (u >= n_users_) throw std::invalid_argument("als score_trained_user: unknown user");
    score_vector(X_.data() + static_cast<std::size_t>(u) * params_.k, out);
  }

  std::vector<std::pair<int, double>> training_curve() const override { return curve_; }
  std::string training_curve_metric() const override { return "objective"; }

 private:
  void require_fitted() const {
    if (!fitted_) throw std::logic_error("als: model is not fitted or loaded");
  }

  void score_vector(const float* x, std::span<float> out) const {
    if (out.size() != n_items_) throw std::invalid_argument("als score: output has the wrong size");
    const std::size_t k = params_.k;
    for (std::uint32_t i = 0; i < n_items_; ++i) {
      if (!has_data_[i]) {
        out[i] = -std::numeric_limits<float>::infinity();
        continue;
      }
      const float* y = Y_.data() + static_cast<std::size_t>(i) * k;
      double s = 0.0;
      for (std::size_t a = 0; a < k; ++a) s += static_cast<double>(x[a]) * static_cast<double>(y[a]);
      out[i] = static_cast<float>(s);
    }
  }

  ad::Params params_;
  bool fitted_ = false;
  std::uint32_t n_items_ = 0;
  std::uint32_t n_users_ = 0;
  std::vector<float> X_;           // n_users x k, row-major (training only; not saved)
  std::vector<float> Y_;           // n_items x k, row-major
  std::vector<double> GY_;         // Y^T Y, k x k, shared by every fold-in
  std::vector<std::uint8_t> has_data_;  // 1 if the item had an interaction in training
  std::vector<std::pair<int, double>> curve_;
};

}  // namespace

std::unique_ptr<Model> make_als() { return std::make_unique<AlsModel>(); }

}  // namespace recs
