// Popularity baselines: "pop" (damped mean rating) and "pop_count" (number of ratings).
//
// Math
// ----
// For item i with n_i ratings and mean rating r̄_i, and C the global mean rating over every
// training rating, "pop" scores
//
//     score_i = (n_i * r̄_i + m * C) / (n_i + m) = (Σ_u r_ui + m * C) / (n_i + m)
//
// This is the mean of item i's ratings after adding m phantom ratings equal to C: a Bayesian
// average (shrinkage towards the prior mean C with prior strength m). With few ratings the score
// stays near C; with many it approaches the item's own mean. So a film rated 5.0 by one person
// does not outrank a film rated 4.3 by thousands. An item with no ratings scores exactly C, and
// m = 0 gives the raw item mean. The spec starts m at 10 (configs/pop.json).
//
// "pop_count" scores score_i = n_i: the most-rated items first. It ignores the rating values, which
// often makes it the stronger ranking baseline (people rate what they watch, and they watch what is
// popular), and it is the reference for the mean-popularity-rank metric.
//
// Both scores are the same for every user: fold_in() only records the user's ratings so that
// recommend() can exclude what the user has already rated. They are the baselines every other
// model must beat on NDCG@10 (spec: Evaluation protocol).
//
// Numerics: sums are accumulated in double (an item in ml-32m has up to ~10^5 ratings, the global
// sum ~3 * 10^7). An empty training set has C = 0, so every item scores 0 (never NaN).
//
// References:
//  - IMDb Top 250 weighted rating, WR = (v / (v + m)) R + (m / (v + m)) C.
//  - C. Aggarwal, "Recommender Systems: The Textbook", Springer 2016, sec. 2.2 (baselines).
//  - P. Cremonesi, Y. Koren, R. Turrin, "Performance of Recommender Algorithms on Top-N
//    Recommendation Tasks", RecSys 2010 (popularity as a strong top-N baseline).
//
// Saved files (inside the model's directory): scores.bin — magic "RECSPOP1", u32 version 1,
// u32 variant (0 = pop, 1 = pop_count), f64 m, f64 C, u32 n_items, then n_items x u32 counts and
// n_items x f32 scores (little-endian).
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "artifact/binio.hpp"
#include "recs/model.hpp"

namespace recs {
namespace {

constexpr std::string_view kMagic = "RECSPOP1";
constexpr std::uint32_t kVersion = 1;
constexpr double kDefaultM = 10.0;

class Popularity final : public Model {
 public:
  explicit Popularity(bool count_variant) : count_variant_(count_variant) {}

  std::string_view name() const override { return count_variant_ ? "pop_count" : "pop"; }

  void fit(const Dataset& train, const Config& cfg, const TrainContext& /*ctx*/) override {
    m_ = parse_config(cfg);
    const std::uint32_t n_items = train.n_items();
    if (train.by_item.n_rows != n_items) throw std::invalid_argument(std::string(name()) + ": by_item rows != n_items");
    counts_.assign(n_items, 0);
    std::vector<double> sums(n_items, 0.0);
    double total = 0.0;
    const Csr& by_item = train.by_item;
    for (std::uint32_t i = 0; i < by_item.n_rows; ++i) {
      double s = 0.0;
      for (float v : by_item.row_values(i)) s += v;
      counts_[i] = by_item.row_size(i);
      sums[i] = s;
      total += s;
    }
    const std::uint64_t n_ratings = by_item.nnz();
    global_mean_ = n_ratings == 0 ? 0.0 : total / static_cast<double>(n_ratings);

    scores_.assign(n_items, 0.0f);
    for (std::uint32_t i = 0; i < n_items; ++i) {
      if (count_variant_) {
        scores_[i] = static_cast<float>(counts_[i]);
      } else {
        const double n = counts_[i];
        // n + m == 0 only when m == 0 and the item is unrated: fall back to C.
        const double s = (n + m_ == 0.0) ? global_mean_ : (sums[i] + m_ * global_mean_) / (n + m_);
        scores_[i] = static_cast<float>(s);
      }
    }
  }

  UserState fold_in(std::span<const Rating> ratings) const override {
    UserState u;
    u.ratings.assign(ratings.begin(), ratings.end());
    return u;
  }

  void score(const UserState& /*u*/, std::span<float> out) const override {
    if (out.size() != scores_.size()) throw std::invalid_argument("score: out.size() != n_items");
    std::copy(scores_.begin(), scores_.end(), out.begin());
  }

  Explanation explain(const UserState& /*u*/, ItemIdx /*item*/) const override { return {}; }

  void save(const fs::path& dir) const override {
    binio::Writer w(dir / "scores.bin", kMagic, kVersion);
    w.u32(count_variant_ ? 1 : 0);
    w.f64(m_);
    w.f64(global_mean_);
    w.u32(static_cast<std::uint32_t>(scores_.size()));
    for (auto c : counts_) w.u32(c);
    for (auto s : scores_) w.f32(s);
    w.close();
  }

  void load(const fs::path& dir) override {
    binio::Reader r(dir / "scores.bin", kMagic, kVersion);
    if (r.u32() != (count_variant_ ? 1u : 0u)) {
      throw ArtifactError(std::string(name()) + ": scores.bin belongs to the other popularity variant");
    }
    const double m = r.f64();
    const double c = r.f64();
    if (!std::isfinite(m) || m < 0.0 || !std::isfinite(c)) {
      throw ArtifactError(std::string(name()) + ": invalid m or global mean in scores.bin");
    }
    const std::uint32_t n = r.u32();
    r.need_elems(n, 8);
    std::vector<std::uint32_t> counts(n);
    std::vector<float> scores(n);
    for (auto& v : counts) v = r.u32();
    for (auto& v : scores) {
      v = r.f32();
      // fit() only writes finite scores; ±inf (or NaN) means the file is corrupt.
      if (!std::isfinite(v)) throw ArtifactError(std::string(name()) + ": non-finite score in scores.bin");
    }
    r.finish();
    m_ = m;
    global_mean_ = c;
    counts_ = std::move(counts);
    scores_ = std::move(scores);
  }

  std::uint32_t n_items() const override { return static_cast<std::uint32_t>(scores_.size()); }

  Config config() const override {
    if (count_variant_) return Config::object();
    return Config{{"m", m_}};
  }

 private:
  // pop: {"m": number >= 0} (default 10). pop_count: no keys. Unknown keys are rejected so that a
  // typo in a config file cannot silently fall back to a default.
  double parse_config(const Config& cfg) const {
    const std::string who(name());
    if (cfg.is_null()) return kDefaultM;
    if (!cfg.is_object()) throw std::invalid_argument(who + ": config must be a JSON object");
    double m = kDefaultM;
    for (const auto& [key, value] : cfg.items()) {
      if (!count_variant_ && key == "m") {
        if (!value.is_number()) throw std::invalid_argument(who + ": m must be a number");
        m = value.get<double>();
        if (!std::isfinite(m) || m < 0.0) throw std::invalid_argument(who + ": m must be finite and >= 0");
      } else {
        throw std::invalid_argument(who + ": unknown config key '" + key + "'");
      }
    }
    return count_variant_ ? 0.0 : m;
  }

  bool count_variant_;
  double m_ = kDefaultM;
  double global_mean_ = 0.0;
  std::vector<std::uint32_t> counts_;
  std::vector<float> scores_;
};

}  // namespace

std::unique_ptr<Model> make_popularity() { return std::make_unique<Popularity>(false); }
std::unique_ptr<Model> make_popularity_count() { return std::make_unique<Popularity>(true); }

}  // namespace recs
