// Item-KNN on real MovieLens data: fit time and memory, plus a diagnostic NDCG@10 comparison with
// a rating-count baseline. Excluded from the default run ([slow] / [ml32m]); run explicitly, e.g.
//   build/debug/tests/recs_tests "[knn][slow]"
//   OMP_NUM_THREADS=4 build/release/tests/recs_tests "[knn][ml32m]"
// The official Phase 1 gate (KNN beats popularity on ml-32m, paired bootstrap) belongs to the
// evaluator. The comparison here only PRINTS numbers for the four scoring modes; it deliberately
// asserts nothing about relative quality.
#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../../../core/src/models/item_knn_detail.hpp"
#include "fixtures.hpp"
#include "recs/dataset.hpp"
#include "recs/metrics.hpp"
#include "recs/model.hpp"
#include "recs/split.hpp"

namespace fs = std::filesystem;
using recs::ItemIdx;

namespace {

// A /proc/self/status field in MiB (Linux only; -1 if unavailable).
double proc_status_mib(const std::string& field) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(field + ":", 0) == 0) return std::stod(line.substr(field.size() + 1)) / 1024.0;
  }
  return -1.0;
}

// Resets VmHWM to the current RSS so the next reading is the peak of what follows.
void reset_peak_rss() {
  std::ofstream out("/proc/self/clear_refs");
  out << "5";
}

struct Fitted {
  std::unique_ptr<recs::Model> model;
  double seconds = 0.0;
  double rss_before = 0.0;
  double peak_during = 0.0;
};

Fitted timed_fit(const recs::Dataset& ds, bool normalize = true, bool positive_only = false) {
  Fitted f;
  f.rss_before = proc_status_mib("VmRSS");
  reset_peak_rss();
  f.model = recs::make_item_knn();
  const auto t0 = std::chrono::steady_clock::now();
  f.model->fit(ds,
               recs::Config{{"neighbors", 100}, {"shrinkage", 100}, {"min_support", 20},
                            {"normalize", normalize}, {"positive_only", positive_only}},
               {});
  f.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  f.peak_during = proc_status_mib("VmHWM");
  return f;
}

void report(const std::string& name, const recs::Dataset& ds, const Fitted& f) {
  std::size_t with_list = 0, total = 0;
  for (ItemIdx i = 0; i < ds.n_items(); ++i) {
    const auto n = recs::item_knn_detail::neighbour_list(*f.model, i).size();
    with_list += n > 0 ? 1 : 0;
    total += n;
  }
  WARN(name << ": " << ds.n_ratings() << " ratings, " << ds.n_items() << " items, threads="
            << omp_get_max_threads() << " | fit " << f.seconds << " s | RSS before fit " << f.rss_before
            << " MiB, peak during fit " << f.peak_during << " MiB | items with a list " << with_list
            << ", neighbour entries " << total);
  CHECK(with_list > 0);
}

// Top-10 by score (desc, ItemIdx asc), skipping -inf and the user's training items.
std::vector<ItemIdx> top10(const std::vector<float>& s, const std::vector<char>& seen) {
  std::vector<ItemIdx> cand;
  for (ItemIdx i = 0; i < s.size(); ++i) if (!seen[i] && std::isfinite(s[i])) cand.push_back(i);
  const auto k = std::min<std::size_t>(10, cand.size());
  std::partial_sort(cand.begin(), cand.begin() + static_cast<std::ptrdiff_t>(k), cand.end(),
                    [&](ItemIdx a, ItemIdx b) { return s[a] != s[b] ? s[a] > s[b] : a < b; });
  cand.resize(k);
  return cand;
}

}  // namespace

TEST_CASE("knn on ml-latest-small: fit time, memory and a diagnostic NDCG@10", "[knn][slow]") {
  const auto raw = recs::testing::data_dir() / "raw" / "ml-latest-small";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-latest-small not present at " << raw);
  const auto full = recs::load_movielens(raw, "ml-latest-small");
  const auto fitted_full = timed_fit(full);
  report("ml-latest-small (full)", full, fitted_full);

  const auto split = recs::make_split(full, recs::SplitConfig{});
  const auto& tr = split.train;
  std::vector<float> pop(tr.n_items());
  for (ItemIdx i = 0; i < tr.n_items(); ++i) pop[i] = static_cast<float>(tr.by_item.row_size(i));
  double pop_sum = 0.0;
  for (auto u : split.eval_users) {
    std::vector<char> seen(tr.n_items(), 0);
    for (auto i : tr.by_user.row_indices(u)) seen[i] = 1;
    pop_sum += recs::ndcg_at_k(top10(pop, seen), split.test[u], 10);
  }
  const auto n = static_cast<double>(split.eval_users.size());
  REQUIRE(n > 0);
  WARN("diagnostic NDCG@10, ml-latest-small test split (" << n << " users): rating-count popularity "
       << pop_sum / n);

  for (const bool normalize : {true, false}) {
    for (const bool positive_only : {false, true}) {
      const auto f = timed_fit(tr, normalize, positive_only);
      double knn_sum = 0.0;
      std::size_t short_lists = 0;
      std::vector<float> s(tr.n_items());
      for (auto u : split.eval_users) {
        std::vector<char> seen(tr.n_items(), 0);
        for (auto i : tr.by_user.row_indices(u)) seen[i] = 1;
        f.model->score_trained_user(u, s);
        const auto rk = top10(s, seen);
        short_lists += rk.size() < 10 ? 1 : 0;
        knn_sum += recs::ndcg_at_k(rk, split.test[u], 10);
      }
      WARN("diagnostic NDCG@10, knn normalize=" << normalize << " positive_only=" << positive_only << ": "
           << knn_sum / n << " (users with < 10 knn items: " << short_lists << ")");
    }
  }
}

TEST_CASE("knn on ml-32m: fit time and memory", "[knn][ml32m]") {
  const auto raw = recs::testing::data_dir() / "raw" / "ml-32m";
  if (!fs::exists(raw / "ratings.csv")) SKIP("ml-32m not present at " << raw);
  const auto full = recs::load_movielens(raw, "ml-32m");
  const auto f = timed_fit(full);
  report("ml-32m (full)", full, f);
}
