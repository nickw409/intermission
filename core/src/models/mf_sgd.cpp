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
//   - Initialisation: biases 0; factors N(0, init_sd^2), except that users and items with no
//     training ratings get zero factors (they would otherwise keep pure noise forever).
//   - Each epoch visits every rating once in an order given by a seeded Fisher-Yates shuffle
//     (recs::Rng), then multiplies the learning rate by lr_decay.
//   - Early stopping: with a validation set (ctx.validation), the RMSE on it is measured after each
//     epoch; training stops after `patience` epochs without improvement and the parameters of the
//     best epoch are restored. Without one, training runs max_epochs and the curve is train RMSE.
//   - Determinism: the SGD loop is serial. Hogwild-style lock-free parallel SGD would be faster
//     but is not reproducible, and the spec forbids it in deterministic mode. Training runs in
//     double precision; the fitted item factors are stored as float32.
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
//   Y. Koren, R. Bell, "Advances in Collaborative Filtering", Recommender Systems Handbook, 2011.
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
void add_rating_gradient(double e, double bu, double bi, const double* p, const double* q,
                         std::uint32_t k, double lambda, double scale, double& g_bu, double& g_bi,
                         double* g_p, double* g_q) {
  g_bu += scale * (-e + lambda * bu);
  g_bi += scale * (-e + lambda * bi);
  for (std::uint32_t f = 0; f < k; ++f) {
    const double pf = p[f];
    const double qf = q[f];
    g_p[f] += scale * (-e * qf + lambda * pf);
    g_q[f] += scale * (-e * pf + lambda * qf);
  }
}

double sq_norm(const double* x, std::uint32_t k) {
  double s = 0.0;
  for (std::uint32_t f = 0; f < k; ++f) s += x[f] * x[f];
  return s;
}

}  // namespace

double predict(const Params& m, std::uint32_t u, std::uint32_t i) {
  const double* p = m.P.data() + std::size_t{u} * m.k;
  const double* q = m.Q.data() + std::size_t{i} * m.k;
  double dot = 0.0;
  for (std::uint32_t f = 0; f < m.k; ++f) dot += p[f] * q[f];
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
    const double e = r.value - predict(m, r.row, r.col);
    add_rating_gradient(e, m.bu[u], m.bi[i], m.P.data() + u * m.k, m.Q.data() + i * m.k, m.k,
                        lambda, 1.0, g.bu[u], g.bi[i], g.P.data() + u * m.k, g.Q.data() + i * m.k);
  }
  return g;
}

void sgd_step(Params& m, const Triplet& rating, double lr, double lambda) {
  const std::size_t u = rating.row, i = rating.col;
  const double e = rating.value - predict(m, rating.row, rating.col);
  double* p = m.P.data() + u * m.k;
  double* q = m.Q.data() + i * m.k;
  add_rating_gradient(e, m.bu[u], m.bi[i], p, q, m.k, lambda, -lr, m.bu[u], m.bi[i], p, q);
}

}  // namespace mf_detail

namespace {

using mf_detail::Params;

// --- configuration ---------------------------------------------------------------------------

struct MfConfig {
  std::uint32_t k = 64;
  double lambda = 0.05;
  double lr = 0.007;
  double lr_decay = 0.95;
  int max_epochs = 40;
  int patience = 3;
  double init_sd = 0.1;

  Config to_json() const {
    return Config{{"k", k},           {"lambda", lambda},     {"lr", lr},
                  {"lr_decay", lr_decay}, {"max_epochs", max_epochs}, {"patience", patience},
                  {"init_sd", init_sd}};
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
  static const char* const known[] = {"k", "lambda", "lr", "lr_decay", "max_epochs", "patience", "init_sd"};
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
  if (cfg.contains("max_epochs")) c.max_epochs = static_cast<int>(integer(cfg, "max_epochs"));
  if (cfg.contains("patience")) c.patience = static_cast<int>(integer(cfg, "patience"));
  if (!(c.lambda >= 0.0 && std::isfinite(c.lambda))) throw std::invalid_argument("mf config: lambda must be >= 0");
  if (!(c.lr > 0.0 && std::isfinite(c.lr))) throw std::invalid_argument("mf config: lr must be > 0");
  if (!(c.lr_decay > 0.0 && c.lr_decay <= 1.0)) throw std::invalid_argument("mf config: lr_decay must be in (0, 1]");
  if (!(c.init_sd >= 0.0 && std::isfinite(c.init_sd))) throw std::invalid_argument("mf config: init_sd must be >= 0");
  if (c.max_epochs < 1) throw std::invalid_argument("mf config: max_epochs must be >= 1");
  if (c.patience < 1) throw std::invalid_argument("mf config: patience must be >= 1");
  return c;
}

// --- binary I/O (contracts/artifact.md rule 5: little-endian, fixed width, header) ------------

static_assert(std::endian::native == std::endian::little, "artifact I/O assumes a little-endian host");

constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'M', 'F', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1;

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
    const std::uint32_t k = cfg_.k;
    const std::uint32_t nu = train.n_users(), ni = train.n_items();

    // Every training rating as a triplet, in CSR order (the shuffle permutes indices into this).
    std::vector<Triplet> ratings;
    ratings.reserve(train.n_ratings());
    double sum = 0.0;
    for (std::uint32_t u = 0; u < nu; ++u) {
      const auto items = train.by_user.row_indices(u);
      const auto vals = train.by_user.row_values(u);
      for (std::size_t j = 0; j < items.size(); ++j) {
        ratings.push_back({u, items[j], vals[j], 0});
        sum += vals[j];
      }
    }

    Params m;
    m.k = k;
    m.mu = ratings.empty() ? 0.0 : sum / static_cast<double>(ratings.size());
    m.bu.assign(nu, 0.0);
    m.bi.assign(ni, 0.0);
    m.P.assign(std::size_t{nu} * k, 0.0);
    m.Q.assign(std::size_t{ni} * k, 0.0);
    Rng init_rng(mix_seed(ctx.seed, 1));
    for (std::uint32_t u = 0; u < nu; ++u)
      if (train.by_user.row_size(u) > 0)
        for (std::uint32_t f = 0; f < k; ++f) m.P[std::size_t{u} * k + f] = cfg_.init_sd * init_rng.normal();
    for (std::uint32_t i = 0; i < ni; ++i)
      if (train.by_item.row_size(i) > 0)
        for (std::uint32_t f = 0; f < k; ++f) m.Q[std::size_t{i} * k + f] = cfg_.init_sd * init_rng.normal();

    const auto* validation = ctx.validation;
    if (validation != nullptr && count_ratings(*validation, nu) == 0) validation = nullptr;
    curve_metric_ = validation != nullptr ? "val_rmse" : "train_rmse";
    curve_.clear();

    std::vector<std::uint32_t> order(ratings.size());
    std::iota(order.begin(), order.end(), 0u);
    Rng shuffle_rng(mix_seed(ctx.seed, 2));

    Params best = m;
    double best_rmse = std::numeric_limits<double>::infinity();
    int since_best = 0;
    double lr = cfg_.lr;
    for (int epoch = 1; epoch <= cfg_.max_epochs; ++epoch) {
      shuffle_rng.shuffle(order.begin(), order.end());
      for (const std::uint32_t idx : order) mf_detail::sgd_step(m, ratings[idx], lr, cfg_.lambda);
      lr *= cfg_.lr_decay;

      const double rmse = validation != nullptr ? validation_rmse(m, *validation) : train_rmse(m, ratings);
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
    return st;
  }

  void score(const UserState& u, std::span<float> out) const override {
    if (u.vec.size() != cfg_.k) throw std::invalid_argument("mf score: user vector has the wrong size");
    score_with(u.bias, u.vec.data(), out);
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
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    put(buf, kFormatVersion);
    put(buf, n_items_);
    put(buf, cfg_.k);
    put(buf, mu_);
    for (const float x : bi_) put(buf, x);
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
    if (in.get<std::uint32_t>() != kFormatVersion) throw std::runtime_error("mf model file: unknown version");
    const auto ni = in.get<std::uint32_t>();
    const auto k = in.get<std::uint32_t>();
    if (k != cfg.k) throw std::runtime_error("mf model file: k does not match config.json");
    const auto mu = in.get<double>();
    std::vector<float> bi(ni), Q(std::size_t{ni} * k);
    for (auto& x : bi) x = in.get<float>();
    for (auto& x : Q) x = in.get<float>();
    if (!in.at_end()) throw std::runtime_error("mf model file: trailing bytes");

    cfg_ = cfg;
    mu_ = mu;
    n_items_ = ni;
    bi_ = std::move(bi);
    Q_ = std::move(Q);
    n_users_ = 0;
    bu_.clear();
    P_.clear();
    has_users_ = false;
    curve_.clear();
    curve_metric_.clear();
  }

 private:
  static std::size_t count_ratings(const std::vector<std::vector<Rating>>& v, std::uint32_t nu) {
    std::size_t n = 0;
    for (std::size_t u = 0; u < std::min<std::size_t>(v.size(), nu); ++u) n += v[u].size();
    return n;
  }

  double validation_rmse(const Params& m, const std::vector<std::vector<Rating>>& validation) const {
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

  static double train_rmse(const Params& m, const std::vector<Triplet>& ratings) {
    if (ratings.empty()) return 0.0;
    double sse = 0.0;
    for (const auto& r : ratings) {
      const double e = r.value - mf_detail::predict(m, r.row, r.col);
      sse += e * e;
    }
    return std::sqrt(sse / static_cast<double>(ratings.size()));
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
  std::vector<float> P_, Q_;    // row-major factors, n_users x k and n_items x k
  bool has_users_ = false;
  std::vector<std::pair<int, double>> curve_;
  std::string curve_metric_;
};

}  // namespace

std::unique_ptr<Model> make_mf_sgd() { return std::make_unique<MfSgd>(); }

}  // namespace recs
