// ALS on real MovieLens data. Skipped when the data is not present (CI has none).
//  - ml-latest-small: fold-in parity Jaccard@10 > 0.95 on the temporal split's evaluated users.
//  - [ml32m]: the k = 64, 15-iteration fit on full ml-32m (release builds only): prints the fit
//    time, the objective curve and the parity on a sample of users.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <catch2/catch_test_macros.hpp>

#include "als_test_util.hpp"
#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/metrics.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"

namespace fs = std::filesystem;

namespace {

namespace tals = recs::testing::als;

fs::path data_root() {
  if (const char* env = std::getenv("RECS_DATA_DIR"); env != nullptr && *env != '\0') return fs::path(env);
  return recs::testing::data_dir();
}

double parity(const recs::Model& m, const recs::Dataset& train, std::uint32_t user_stride) {
  double sum = 0.0;
  std::size_t n = 0;
  std::vector<float> a(m.n_items()), b(m.n_items());
  for (recs::UserIdx u = 0; u < train.n_users(); u += user_stride) {
    const auto r = tals::user_ratings(train, u);
    m.score_trained_user(u, a);
    m.score(m.fold_in(r), b);
    sum += recs::jaccard_at_k(tals::top_k(a, r, 10), tals::top_k(b, r, 10), 10);
    ++n;
  }
  return sum / static_cast<double>(n);
}

}  // namespace

TEST_CASE("ALS on ml-latest-small: fold-in parity Jaccard@10 > 0.95", "[als][ml-small]") {
  const auto raw = data_root() / "raw" / "ml-latest-small";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-latest-small not present at " << raw);
  const auto split = recs::make_split(recs::load_movielens(raw, "ml-latest-small"), recs::SplitConfig{});
  auto m = recs::make_model("als");
  m->fit(split.train, {{"k", 16}, {"iters", 6}}, recs::TrainContext{});
  const double j = parity(*m, split.train, 1);
  INFO("mean Jaccard@10 = " << j);
  CHECK(j > 0.95);
}

TEST_CASE("ALS on ml-32m: k = 64, 15 iterations, fit time and parity", "[.][ml32m][als-ml32m]") {
  const auto raw = data_root() / "raw" / "ml-32m";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-32m not present at " << raw);
  using Clock = std::chrono::steady_clock;
  const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };

  const auto t0 = Clock::now();
  const auto ds = recs::load_or_build(raw, data_root() / "cache" / "ml-32m.bin", "ml-32m");
  const auto t1 = Clock::now();
  auto m = recs::make_model("als");
  m->fit(ds, {{"k", 64}, {"iters", 15}}, recs::TrainContext{});
  const auto t2 = Clock::now();
  const double j = parity(*m, ds, 200);
  const auto t3 = Clock::now();

  std::cout << "ml-32m ALS: load " << seconds(t1 - t0) << " s, fit " << seconds(t2 - t1)
            << " s, parity on every 200th user " << seconds(t3 - t2) << " s\n";
  for (const auto& [it, obj] : m->training_curve()) std::cout << "  iteration " << it << " objective " << obj << "\n";
  std::cout << "  mean Jaccard@10 (fold-in vs trained) = " << j << "\n";
  CHECK(j > 0.95);
}
