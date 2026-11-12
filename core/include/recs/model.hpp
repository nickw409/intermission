// The one interface every recommender implements (spec: Models), plus the shared recommend().
// FROZEN after Wave 0.
//
// Contract:
//  - fit() trains on `train`. Models that early-stop read ctx.validation (indexed by UserIdx).
//    With ctx.deterministic, results must be bit-identical across runs with the same seed:
//    static OpenMP schedules only, no lock-free racing updates.
//  - fold_in() is pure and thread-safe: it builds the user's state from ratings alone, keeping
//    every trained item-side parameter fixed. Offline evaluation scores training users through
//    fold_in() too, so the website and the evaluator run the same code path.
//  - score() writes one score per item (out.size() == n_items). Higher is better. NaN is never
//    allowed; use -infinity for "cannot score".
//  - mask_unrankable() sets -infinity for items the model never recommends (a ranking rule such
//    as MF's min_support floor). recommend() applies it after score(); score() itself stays the
//    model's full prediction, so rating metrics (RMSE/MAE) are unaffected.
//  - explain() only ever cites items present in u.ratings.
//  - save()/load() use only files inside `dir` (created by the caller).
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "recs/types.hpp"

namespace recs {

namespace fs = std::filesystem;

/// Model-specific user state. Each model fills the fields it needs and documents which.
struct UserState {
  std::vector<Rating> ratings;  // the ratings the state was built from (dense item ids)
  double mean = 0.0;            // centering offset: damped user mean rating (KNN); μ + b_u (content)
  double bias = 0.0;            // user bias b_u (MF)
  std::vector<float> vec;       // factor vector (MF, ALS) or profile vector (content)
};

struct Because {
  ItemIdx item = 0;
  float weight = 0.0f;  // contribution to the score; larger = more responsible
};

struct Explanation {
  std::vector<Because> because;  // sorted by weight descending; at most 3 entries
};

struct Scored {
  ItemIdx item = 0;
  float score = 0.0f;
  bool operator==(const Scored&) const = default;
};

struct TrainContext {
  const std::vector<std::vector<Rating>>* validation = nullptr;  // indexed by UserIdx, may be null
  std::uint64_t seed = 42;
  bool deterministic = true;
};

class NotImplemented : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

class Model {
 public:
  virtual ~Model() = default;

  virtual std::string_view name() const = 0;
  virtual void fit(const Dataset& train, const Config& cfg, const TrainContext& ctx) = 0;
  virtual UserState fold_in(std::span<const Rating> ratings) const = 0;
  virtual void score(const UserState& u, std::span<float> out) const = 0;
  virtual Explanation explain(const UserState& u, ItemIdx item) const = 0;
  /// Ranking-only mask: set scores[i] = -infinity for every item this model will not recommend
  /// (scores.size() == n_items()). The default ranks everything.
  virtual void mask_unrankable(std::span<float> scores) const { (void)scores; }
  virtual void save(const fs::path& dir) const = 0;
  virtual void load(const fs::path& dir) = 0;

  virtual std::uint32_t n_items() const = 0;

  /// The config fit() actually used (defaults filled in); copied into artifacts and reports.
  virtual Config config() const = 0;

  /// True if score() is a predicted star rating (MF, rating baselines): enables RMSE/MAE.
  virtual bool predicts_ratings() const { return false; }

  /// The batch path: scores for training user `u` computed from what fit() learned or saw
  /// (MF/ALS: the trained user vector; KNN/content: the training matrix row directly), without
  /// going through fold_in(). Used by the fold-in parity check (spec: Jaccard@10; MF > 0.9,
  /// ALS > 0.95) and by the KNN/content gate "fold_in + score equals the batch path exactly".
  /// Models that keep no per-user training information return false from supports_trained_scoring.
  virtual bool supports_trained_scoring() const { return false; }
  virtual void score_trained_user(UserIdx u, std::span<float> out) const {
    (void)u; (void)out;
    throw NotImplemented(std::string(name()) + " has no batch scoring path");
  }

  /// Per-epoch / per-iteration curve recorded during fit(): (iteration, value).
  virtual std::vector<std::pair<int, double>> training_curve() const { return {}; }
  /// Name of the value in training_curve(), e.g. "val_rmse" or "objective".
  virtual std::string training_curve_metric() const { return ""; }

  /// Item-to-item neighbours (item-KNN, content). Others throw NotImplemented.
  virtual std::vector<Scored> similar(ItemIdx item, std::size_t k) const {
    (void)item; (void)k;
    throw NotImplemented(std::string(name()) + " does not support similar()");
  }
  virtual bool supports_similar() const { return false; }
};

/// Shared top-k: score every item, apply m.mask_unrankable(), mask `exclude` (and every item in
/// u.ratings), take the k best with nth_element, then sort. Order: score descending, then ItemIdx
/// ascending (deterministic ties). Items scored -infinity are never returned, so the result may be
/// shorter than k.
std::vector<Scored> recommend(const Model& m, const UserState& u, std::size_t k,
                              std::span<const ItemIdx> exclude = {});

// Factories. The registry (core/src/models/registry.cpp) is the only place that lists models.
std::unique_ptr<Model> make_popularity();        // "pop": damped mean
std::unique_ptr<Model> make_popularity_count();  // "pop_count": rating count
std::unique_ptr<Model> make_item_knn();          // "knn"
std::unique_ptr<Model> make_content();           // "content"
std::unique_ptr<Model> make_mf_sgd();            // "mf"
std::unique_ptr<Model> make_als();               // "als"

/// Creates a model by name; throws std::invalid_argument for an unknown name.
std::unique_ptr<Model> make_model(std::string_view name);
/// Every registered model name, in display order.
std::vector<std::string> model_names();

}  // namespace recs
