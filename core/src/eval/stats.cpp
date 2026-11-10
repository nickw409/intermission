// STUB (Wave 0). Owner: workstream B. Replace with the real implementation.
#include "recs/stats.hpp"
#include "util/todo.hpp"
namespace recs {
Estimate bootstrap_ci(std::span<const double>, std::size_t, std::uint64_t, double) { RECS_TODO("bootstrap_ci"); }
PairedComparison paired_bootstrap(std::span<const double>, std::span<const double>, std::size_t, std::uint64_t, double) { RECS_TODO("paired_bootstrap"); }
bool cis_disjoint(const Estimate&, const Estimate&) { RECS_TODO("cis_disjoint"); }
Estimate bootstrap_ratio_ci(std::span<const double>, std::span<const double>, std::size_t, std::uint64_t, double, bool) { RECS_TODO("bootstrap_ratio_ci"); }
}  // namespace recs
