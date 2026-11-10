// Bootstrap confidence intervals over users (spec: Statistical honesty).
// Workstream B owns the implementation. FROZEN after Wave 0.
// Percentile bootstrap: draw `resamples` resamples of the per-user values with replacement using
// recs::Rng(seed); the CI is the [(1-level)/2, (1+level)/2] percentiles of the resample means
// (linear interpolation between order statistics). Accumulate in double.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace recs {

struct Estimate {
  double mean = 0.0;
  double lo = 0.0;
  double hi = 0.0;
};

Estimate bootstrap_ci(std::span<const double> per_user, std::size_t resamples = 1000,
                      std::uint64_t seed = 42, double level = 0.95);

struct PairedComparison {
  double mean_delta = 0.0;  // mean(a - b)
  double lo = 0.0;
  double hi = 0.0;
  bool excludes_zero = false;  // lo > 0 or hi < 0
};

/// Paired bootstrap on per-user deltas a[i] - b[i]. a and b must be the same users in the same order.
PairedComparison paired_bootstrap(std::span<const double> a, std::span<const double> b,
                                  std::size_t resamples = 1000, std::uint64_t seed = 42,
                                  double level = 0.95);

/// True iff the two CIs do not overlap.
bool cis_disjoint(const Estimate& a, const Estimate& b);

}  // namespace recs

namespace recs {

/// Bootstrap CI for a ratio of per-user sums: sum(num) / sum(den), or sqrt of that if
/// `sqrt_result` (RMSE from per-user sum of squared errors and counts). Same resampling as above.
Estimate bootstrap_ratio_ci(std::span<const double> num, std::span<const double> den,
                            std::size_t resamples = 1000, std::uint64_t seed = 42,
                            double level = 0.95, bool sqrt_result = false);

}  // namespace recs
