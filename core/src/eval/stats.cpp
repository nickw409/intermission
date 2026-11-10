// Bootstrap confidence intervals over users (spec: Statistical honesty; recs/stats.hpp).
//
// The evaluation unit is the user: each evaluated user contributes one value (e.g. their NDCG@10),
// and the uncertainty we report is how much the mean would move if we had drawn a different set
// of users. The nonparametric bootstrap (Efron, 1979; Efron & Tibshirani, "An Introduction to the
// Bootstrap", 1993, ch. 13) estimates that by resampling the n users with replacement:
//
//   for b = 1..B:  draw n indices uniformly from [0, n) with recs::Rng(seed)
//                  theta*_b = statistic of the resampled users
//   CI = [ q_{(1-level)/2}, q_{(1+level)/2} ] of theta*_1..theta*_B     (percentile method)
//
// The statistic is the mean for bootstrap_ci, the mean of per-user deltas a_i - b_i for
// paired_bootstrap (pairing removes the between-user variance both models share), and
// sum(num) / sum(den) (optionally square-rooted) for bootstrap_ratio_ci, e.g. RMSE pooled over
// ratings but resampled by user.
//
// Quantiles use linear interpolation between order statistics (Hyndman & Fan, 1996, definition 7;
// the default of R's quantile() and numpy.percentile): with the B values sorted ascending as
// x_0 <= ... <= x_{B-1}, q_p = x_f + (h - f) * (x_{f+1} - x_f) where h = (B - 1) * p, f = floor(h).
//
// Every resampling function draws the same index stream for the same (n, B, seed), so the
// statistics are comparable across calls. Everything accumulates in double.
#include "recs/stats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "recs/rng.hpp"

namespace recs {

namespace {

void check_args(std::size_t n, std::size_t resamples, double level, const char* who) {
  if (n == 0) throw std::invalid_argument(std::string(who) + ": no values");
  if (resamples == 0) throw std::invalid_argument(std::string(who) + ": resamples must be > 0");
  if (!(level > 0.0 && level < 1.0)) {
    throw std::invalid_argument(std::string(who) + ": level must be in (0, 1)");
  }
}

// Linear interpolation between order statistics (Hyndman & Fan definition 7). `sorted` is ascending.
double quantile_sorted(const std::vector<double>& sorted, double p) {
  const double h = static_cast<double>(sorted.size() - 1) * p;
  const auto f = static_cast<std::size_t>(std::floor(h));
  if (f + 1 >= sorted.size()) return sorted.back();
  const double frac = h - static_cast<double>(f);
  return sorted[f] + frac * (sorted[f + 1] - sorted[f]);
}

// Runs `resamples` bootstrap resamples of n users. For each resample, `stat` receives a callback
// that yields the n drawn indices in order; it returns the statistic. Returns the sorted statistics.
template <class Stat>
std::vector<double> resample_sorted(std::size_t n, std::size_t resamples, std::uint64_t seed, Stat stat) {
  Rng rng(seed);
  std::vector<double> out(resamples);
  for (std::size_t b = 0; b < resamples; ++b) {
    out[b] = stat([&] { return static_cast<std::size_t>(rng.uniform(n)); });
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::pair<double, double> percentile_ci(const std::vector<double>& sorted, double level) {
  return {quantile_sorted(sorted, (1.0 - level) / 2.0), quantile_sorted(sorted, (1.0 + level) / 2.0)};
}

}  // namespace

Estimate bootstrap_ci(std::span<const double> per_user, std::size_t resamples, std::uint64_t seed,
                      double level) {
  const std::size_t n = per_user.size();
  check_args(n, resamples, level, "bootstrap_ci");
  const double dn = static_cast<double>(n);

  Estimate e;
  double sum = 0.0;
  for (double v : per_user) sum += v;
  e.mean = sum / dn;

  const auto stats = resample_sorted(n, resamples, seed, [&](auto draw) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += per_user[draw()];
    return s / dn;
  });
  std::tie(e.lo, e.hi) = percentile_ci(stats, level);
  return e;
}

PairedComparison paired_bootstrap(std::span<const double> a, std::span<const double> b,
                                  std::size_t resamples, std::uint64_t seed, double level) {
  if (a.size() != b.size()) {
    throw std::invalid_argument("paired_bootstrap: a and b must hold the same users");
  }
  std::vector<double> delta(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) delta[i] = a[i] - b[i];
  const Estimate e = bootstrap_ci(delta, resamples, seed, level);
  PairedComparison p;
  p.mean_delta = e.mean;
  p.lo = e.lo;
  p.hi = e.hi;
  p.excludes_zero = p.lo > 0.0 || p.hi < 0.0;
  return p;
}

bool cis_disjoint(const Estimate& a, const Estimate& b) { return a.hi < b.lo || b.hi < a.lo; }

Estimate bootstrap_ratio_ci(std::span<const double> num, std::span<const double> den,
                            std::size_t resamples, std::uint64_t seed, double level,
                            bool sqrt_result) {
  const std::size_t n = num.size();
  if (den.size() != n) throw std::invalid_argument("bootstrap_ratio_ci: num and den differ in size");
  check_args(n, resamples, level, "bootstrap_ratio_ci");
  double total_num = 0.0, total_den = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (den[i] < 0.0) throw std::invalid_argument("bootstrap_ratio_ci: negative denominator");
    total_num += num[i];
    total_den += den[i];
  }
  if (!(total_den > 0.0)) throw std::invalid_argument("bootstrap_ratio_ci: denominators sum to 0");

  auto finish = [sqrt_result](double ratio) { return sqrt_result ? std::sqrt(ratio) : ratio; };
  Estimate e;
  e.mean = finish(total_num / total_den);

  // The statistic of each resample is finish(sum num / sum den), and its percentiles form the CI.
  const auto stats = resample_sorted(n, resamples, seed, [&](auto draw) {
    double sn = 0.0, sd = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = draw();
      sn += num[j];
      sd += den[j];
    }
    if (!(sd > 0.0)) {
      throw std::domain_error("bootstrap_ratio_ci: a resample drew only zero denominators");
    }
    return finish(sn / sd);
  });
  std::tie(e.lo, e.hi) = percentile_ci(stats, level);
  return e;
}

}  // namespace recs
