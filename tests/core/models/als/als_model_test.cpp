// Implicit ALS through the Model interface: planted-structure AUC gate, fold-in parity,
// determinism, explanations, save/load, config handling, and the planted_implicit generator.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <omp.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "../../../../core/src/models/als_detail.hpp"
#include "als_test_util.hpp"
#include "property.hpp"
#include "recs/metrics.hpp"
#include "recs/model.hpp"
#include "recs/synthetic.hpp"

namespace fs = std::filesystem;

namespace {

using recs::Config;
using recs::Dataset;
using recs::Rating;
using recs::Rng;
namespace ad = recs::als_detail;
namespace tals = recs::testing::als;

constexpr float kNegInf = -std::numeric_limits<float>::infinity();

std::unique_ptr<recs::Model> fit_als(const Dataset& ds, const Config& cfg, std::uint64_t seed = 7) {
  auto m = recs::make_model("als");
  recs::TrainContext ctx;
  ctx.seed = seed;
  m->fit(ds, cfg, ctx);
  return m;
}

std::vector<float> fold_in_scores(const recs::Model& m, std::span<const Rating> ratings) {
  std::vector<float> s(m.n_items());
  m.score(m.fold_in(ratings), s);
  return s;
}

std::vector<float> trained_scores(const recs::Model& m, recs::UserIdx u) {
  std::vector<float> s(m.n_items());
  m.score_trained_user(u, s);
  return s;
}

class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    path_ = fs::temp_directory_path() / ("recs-als-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void spit(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// planted_implicit

TEST_CASE("planted_implicit: shapes, disjoint held-out positives, determinism", "[als][synthetic]") {
  const auto a = recs::synthetic::planted_implicit(200, 150, 5, 0.06, 0.25, 11);
  const auto b = recs::synthetic::planted_implicit(200, 150, 5, 0.06, 0.25, 11);
  const auto c = recs::synthetic::planted_implicit(200, 150, 5, 0.06, 0.25, 12);
  CHECK(a.rank == 5);
  CHECK(a.train.n_users() == 200);
  CHECK(a.train.n_items() == 150);
  REQUIRE(a.heldout_positive.size() == 200);
  CHECK(a.train.by_user == b.train.by_user);
  CHECK(a.heldout_positive == b.heldout_positive);
  CHECK_FALSE(a.train.by_user == c.train.by_user);

  std::size_t total = 0;
  for (std::uint32_t u = 0; u < 200; ++u) {
    const auto items = a.train.by_user.row_indices(u);
    const auto values = a.train.by_user.row_values(u);
    REQUIRE(!items.empty());
    for (float v : values) CHECK(v == 5.0f);
    const std::set<recs::ItemIdx> train_set(items.begin(), items.end());
    for (const auto& r : a.heldout_positive[u]) {
      CHECK(r.item < 150);
      CHECK(r.value == 5.0f);
      CHECK(train_set.count(r.item) == 0);
    }
    const std::size_t n = items.size() + a.heldout_positive[u].size();
    CHECK(n == 9);  // round(0.06 * 150)
    CHECK(a.heldout_positive[u].size() == 2);  // round(0.25 * 9)
    total += n;
  }
  CHECK(total == 200u * 9u);
}

// ---------------------------------------------------------------------------------------------
// Gates

TEST_CASE("ALS gate: planted rank-5 structure is recovered with mean AUC > 0.9", "[als][gate]") {
  // Fit settings were picked on a separate tuning seed (909), never on the gate seeds below:
  // grid k in {5, 8}, lambda in {0.1, 1, 10, 50}, alpha in {1, 5, 20, 40}. On seed 909 every
  // point scored 0.90-0.94 except lambda = 50, alpha = 1 (0.75); the best, k = 8, lambda = 50,
  // alpha = 40, scored 0.9415. The generator's oracle AUC (ranking by the true affinities) is
  // about 0.96 on these seeds; see implicit.cpp.
  const Config cfg = {{"k", 8}, {"lambda", 50.0}, {"alpha", 40.0}, {"iters", 15}};
  for (std::uint64_t seed : {101ull, 202ull, 303ull}) {
    INFO("data seed " << seed);
    const auto planted = recs::synthetic::planted_implicit(1000, 400, 5, 0.05, 0.2, seed);
    const auto m = fit_als(planted.train, cfg, seed);
    double sum = 0.0;
    std::size_t users = 0;
    for (recs::UserIdx u = 0; u < planted.train.n_users(); ++u) {
      if (planted.heldout_positive[u].empty()) continue;
      const auto train = tals::user_ratings(planted.train, u);
      const auto scores = fold_in_scores(*m, train);
      std::vector<recs::ItemIdx> pos, excl;
      for (const auto& r : planted.heldout_positive[u]) pos.push_back(r.item);
      for (const auto& r : train) excl.push_back(r.item);
      sum += recs::roc_auc(scores, pos, excl);
      ++users;
    }
    REQUIRE(users > 900);
    const double auc = sum / static_cast<double>(users);
    INFO("mean AUC = " << auc);
    CHECK(auc > 0.9);
  }
}

TEST_CASE("ALS gate (fast proxy): fold-in parity Jaccard@10 > 0.95 against trained user vectors", "[als][gate]") {
  SECTION("planted data") {
    const auto planted = recs::synthetic::planted_implicit(500, 300, 5, 0.05, 0.2, 77);
    const auto m = fit_als(planted.train, {{"k", 16}, {"iters", 10}});
    REQUIRE(m->supports_trained_scoring());
    double sum = 0.0;
    for (recs::UserIdx u = 0; u < planted.train.n_users(); ++u) {
      const auto train = tals::user_ratings(planted.train, u);
      const auto a = tals::top_k(trained_scores(*m, u), train, 10);
      const auto b = tals::top_k(fold_in_scores(*m, train), train, 10);
      sum += recs::jaccard_at_k(a, b, 10);
    }
    const double j = sum / planted.train.n_users();
    INFO("mean Jaccard@10 = " << j);
    CHECK(j > 0.95);
  }
  SECTION("explicit ratings with some below the threshold, users without interactions") {
    Rng rng(78);
    const Dataset ds = tals::random_ratings(300, 120, 0.08, rng);
    const auto m = fit_als(ds, {{"k", 12}, {"iters", 8}});
    double sum = 0.0;
    for (recs::UserIdx u = 0; u < ds.n_users(); ++u) {
      const auto train = tals::user_ratings(ds, u);
      const auto ts = trained_scores(*m, u);
      const auto fs_ = fold_in_scores(*m, train);
      sum += recs::jaccard_at_k(tals::top_k(ts, train, 10), tals::top_k(fs_, train, 10), 10);
    }
    const double j = sum / ds.n_users();
    INFO("mean Jaccard@10 = " << j);
    CHECK(j > 0.95);
  }
}

// ---------------------------------------------------------------------------------------------
// Determinism and training curve

TEST_CASE("ALS fit is deterministic per seed: bitwise factors with 1 vs 4 threads", "[als]") {
  // More users and items than one Gram block (kBlockRows = 1024 in als.cpp), so the blocked,
  // multi-block Gram sums run, and more rows than threads x chunk so every thread gets work.
  const auto planted = recs::synthetic::planted_implicit(2600, 1500, 5, 0.02, 0.2, 5);
  REQUIRE(planted.train.n_users() > 2 * 1024);
  REQUIRE(planted.train.n_items() > 1024);
  const Config cfg = {{"k", 10}, {"iters", 3}};

  struct Run {
    std::unique_ptr<recs::Model> model;
    std::vector<float> X, Y;  // final factors, captured through the half-step hook
  };
  const auto run = [&](int threads, std::uint64_t seed) {
    Run r;
    ad::set_half_step_hook([&](const ad::HalfStepEvent& e) {
      r.X.assign(e.X.begin(), e.X.end());
      r.Y.assign(e.Y.begin(), e.Y.end());
    });
    const int saved = omp_get_max_threads();
    omp_set_num_threads(threads);
    try {
      r.model = fit_als(planted.train, cfg, seed);
    } catch (...) {
      omp_set_num_threads(saved);
      ad::set_half_step_hook({});
      throw;
    }
    omp_set_num_threads(saved);
    ad::set_half_step_hook({});
    return r;
  };
  const Run one = run(1, 9);
  const Run four = run(4, 9);
  const Run other_seed = run(4, 10);

  REQUIRE(one.X.size() == static_cast<std::size_t>(planted.train.n_users()) * 10);
  REQUIRE(one.Y.size() == static_cast<std::size_t>(planted.train.n_items()) * 10);
  CHECK(std::memcmp(one.X.data(), four.X.data(), one.X.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(one.Y.data(), four.Y.data(), one.Y.size() * sizeof(float)) == 0);
  CHECK(one.model->training_curve() == four.model->training_curve());
  CHECK(one.X != other_seed.X);
  CHECK(one.model->training_curve() != other_seed.model->training_curve());
  for (recs::UserIdx u = 0; u < planted.train.n_users(); u += 97)
    CHECK(trained_scores(*one.model, u) == trained_scores(*four.model, u));
}

TEST_CASE("ALS records the objective after every iteration and it does not increase", "[als]") {
  const auto planted = recs::synthetic::planted_implicit(300, 200, 5, 0.05, 0.2, 6);
  const auto m = fit_als(planted.train, {{"k", 8}, {"iters", 7}});
  CHECK(m->training_curve_metric() == "objective");
  const auto curve = m->training_curve();
  REQUIRE(curve.size() == 7);
  for (std::size_t i = 0; i < curve.size(); ++i) {
    CHECK(curve[i].first == static_cast<int>(i + 1));
    CHECK(std::isfinite(curve[i].second));
    if (i > 0) CHECK(curve[i].second <= curve[i - 1].second * (1.0 + 1e-9));
  }
}

// ---------------------------------------------------------------------------------------------
// Scoring, fold-in edge cases, explanations

TEST_CASE("ALS scoring: items without interactions are -inf, never NaN; bad inputs throw", "[als]") {
  // Item 3 is only rated below the threshold; item 4 is never rated.
  const Dataset ds = recs::make_dataset("t", 4, 5,
                                        {{0, 0, 5.0f, 0}, {0, 1, 4.0f, 0}, {1, 1, 3.5f, 0}, {1, 2, 4.5f, 0},
                                         {2, 0, 4.0f, 0}, {2, 3, 2.0f, 0}, {3, 3, 3.0f, 0}});
  const auto m = fit_als(ds, {{"k", 2}, {"iters", 3}});
  CHECK(m->name() == "als");
  CHECK(m->n_items() == 5);
  CHECK_FALSE(m->predicts_ratings());

  const auto s = fold_in_scores(*m, tals::user_ratings(ds, 0));
  for (int i = 0; i < 3; ++i) CHECK(std::isfinite(s[i]));
  CHECK(s[3] == kNegInf);
  CHECK(s[4] == kNegInf);

  // User 3 has no interactions: a zero vector, finite zero scores for warm items.
  const auto st = m->fold_in(tals::user_ratings(ds, 3));
  CHECK(st.vec == std::vector<float>(2, 0.0f));
  CHECK(st.ratings == tals::user_ratings(ds, 3));
  const auto s3 = fold_in_scores(*m, tals::user_ratings(ds, 3));
  CHECK(s3[0] == 0.0f);

  // Duplicates and order do not matter.
  const std::vector<Rating> shuffled{{1, 4.0f, 0}, {0, 5.0f, 0}, {1, 4.0f, 0}};
  CHECK(m->fold_in(shuffled).vec == m->fold_in(tals::user_ratings(ds, 0)).vec);

  std::vector<float> wrong(4);
  CHECK_THROWS_AS(m->score(st, wrong), std::invalid_argument);
  const std::vector<Rating> bad_item{{9, 5.0f, 0}};
  CHECK_THROWS_AS(m->fold_in(bad_item), std::invalid_argument);
  std::vector<float> out(5);
  CHECK_THROWS_AS(m->score_trained_user(4, out), std::invalid_argument);
}

TEST_CASE("ALS explain cites only the user's interactions; contributions sum to the score", "[als]") {
  const auto planted = recs::synthetic::planted_implicit(300, 150, 5, 0.06, 0.2, 21);
  const Config cfg = {{"k", 8}, {"iters", 6}};
  const auto m = fit_als(planted.train, cfg);
  const auto p = ad::parse_config(m->config());
  for (recs::UserIdx u = 0; u < 20; ++u) {
    auto ratings = tals::user_ratings(planted.train, u);
    ratings.push_back({static_cast<recs::ItemIdx>((u * 7) % 150), 1.0f, 0});  // a low rating: never cited
    const auto st = m->fold_in(ratings);
    const auto scores = fold_in_scores(*m, ratings);
    const auto top = tals::top_k(scores, ratings, 3);
    for (recs::ItemIdx item : top) {
      const auto e = m->explain(st, item);
      CHECK(e.because.size() <= 3);
      for (std::size_t j = 0; j < e.because.size(); ++j) {
        const auto& b = e.because[j];
        const bool cited_interaction = std::any_of(ratings.begin(), ratings.end(), [&](const Rating& r) {
          return r.item == b.item && r.value >= 3.5f;
        });
        CHECK(cited_interaction);
        CHECK(b.weight > 0.0f);
        if (j > 0) CHECK(e.because[j - 1].weight >= b.weight);
      }
    }
  }
  // The full decomposition behind explain() reproduces the score (detail API, known factors).
  Rng rng(3);
  const std::size_t k = 6, n = 40;
  const auto F = tals::random_factors(n, k, 0.5, rng);
  const auto G = ad::gram(F, n, k);
  const std::vector<std::uint32_t> rows{1, 5, 9, 17, 30};
  const auto x = ad::solve_row(G, F, k, rows, p.alpha, p.lambda);
  for (std::uint32_t item = 0; item < n; ++item) {
    const auto c = ad::contributions(G, F, k, rows, p.alpha, p.lambda, item);
    REQUIRE(c.size() == rows.size());
    double sum = 0.0, dot = 0.0;
    for (double v : c) sum += v;
    for (std::size_t a = 0; a < k; ++a) dot += x[a] * F[item * k + a];
    CHECK(std::abs(sum - dot) <= 1e-9 * std::max(1.0, std::abs(dot)));
  }
  CHECK_THROWS_AS(m->explain(m->fold_in({}), 150), std::invalid_argument);
}

// ---------------------------------------------------------------------------------------------
// Config

TEST_CASE("ALS config: defaults from configs/als.json, validation, echo", "[als]") {
  const auto d = ad::parse_config(Config::object());
  CHECK(d.k == 64);
  CHECK(d.lambda == 0.05);
  CHECK(d.alpha == 40.0);
  CHECK(d.iters == 15);
  CHECK(d.interaction_threshold == 3.5f);
  CHECK(d.init_sd == 0.01);
  CHECK(ad::parse_config(ad::to_config(d)).k == 64);

  CHECK_THROWS_AS(ad::parse_config({{"k", 0}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"lambda", 0.0}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"alpha", -1.0}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"iters", -1}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"init_sd", 0.0}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"k", "8"}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config({{"lamda", 0.1}}), std::invalid_argument);
  CHECK_THROWS_AS(ad::parse_config(Config::array()), std::invalid_argument);

  const auto planted = recs::synthetic::planted_implicit(50, 40, 3, 0.1, 0.2, 1);
  const auto m = fit_als(planted.train, {{"k", 4}, {"iters", 2}});
  const auto c = m->config();
  CHECK(c.at("k") == 4);
  CHECK(c.at("iters") == 2);
  CHECK(c.at("alpha") == 40.0);
  CHECK(c.contains("interaction_threshold"));

  auto unfit = recs::make_model("als");
  std::vector<float> out(1);
  CHECK_THROWS_AS(unfit->score(recs::UserState{}, out), std::logic_error);
  CHECK_FALSE(unfit->supports_trained_scoring());
}

// ---------------------------------------------------------------------------------------------
// save / load

TEST_CASE("ALS save/load round-trips fold-in scores bit for bit and rejects hostile files", "[als]") {
  const auto planted = recs::synthetic::planted_implicit(200, 120, 5, 0.06, 0.2, 31);
  const auto m = fit_als(planted.train, {{"k", 8}, {"iters", 4}});
  TempDir dir("saveload");
  m->save(dir.path());

  auto loaded = recs::make_model("als");
  loaded->load(dir.path());
  CHECK(loaded->n_items() == m->n_items());
  CHECK(loaded->config() == m->config());
  CHECK_FALSE(loaded->supports_trained_scoring());  // per-user factors are not saved
  for (recs::UserIdx u = 0; u < 50; ++u) {
    const auto r = tals::user_ratings(planted.train, u);
    CHECK(fold_in_scores(*loaded, r) == fold_in_scores(*m, r));
    CHECK(fold_in_scores(*loaded, r) == trained_scores(*m, u));
  }

  // Every file the model wrote stays inside dir.
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir.path()))
    if (e.is_regular_file()) files.push_back(e.path());
  REQUIRE(!files.empty());

  const fs::path factors = dir.path() / "factors.bin";
  const fs::path config = dir.path() / "config.json";
  REQUIRE(fs::exists(factors));
  REQUIRE(fs::exists(config));
  const std::string good_factors = slurp(factors);
  const std::string good_config = slurp(config);

  auto expect_reject = [&](const std::string& what) {
    INFO(what);
    auto fresh = recs::make_model("als");
    CHECK_THROWS(fresh->load(dir.path()));
    spit(factors, good_factors);
    spit(config, good_config);
  };

  spit(factors, good_factors.substr(0, good_factors.size() - 3));
  expect_reject("truncated factors");
  spit(factors, good_factors + "x");
  expect_reject("trailing bytes");
  {
    std::string bad = good_factors;
    bad[0] = 'X';
    spit(factors, bad);
  }
  expect_reject("bad magic");
  {
    std::string bad = good_factors;
    bad[8] = 9;  // format version
    spit(factors, bad);
  }
  expect_reject("unknown version");
  {
    std::string bad = good_factors;
    bad[12] = static_cast<char>(0xFF);  // n_items low byte
    bad[13] = static_cast<char>(0xFF);
    bad[14] = static_cast<char>(0xFF);
    bad[15] = static_cast<char>(0x7F);
    spit(factors, bad);
  }
  expect_reject("huge n_items");
  {
    std::string bad = good_factors;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bad.data() + bad.size() - sizeof(float), &nan, sizeof(float));
    spit(factors, bad);
  }
  expect_reject("NaN factor");
  {
    std::string bad = good_factors;
    bad[20] = 7;  // first has-data flag must be 0 or 1
    spit(factors, bad);
  }
  expect_reject("bad flag");
  spit(config, "{\"k\": 9}");
  expect_reject("config k disagrees with factors");
  spit(config, "not json");
  expect_reject("config not json");
  fs::remove(factors);
  expect_reject("missing factors");

  auto ok = recs::make_model("als");
  REQUIRE_NOTHROW(ok->load(dir.path()));
}
