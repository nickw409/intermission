// STUB (Wave 0). Owner: workstream B. Replace with the real implementation.
#include "recs/metrics.hpp"
#include "util/todo.hpp"
namespace recs {
std::size_t count_relevant(std::span<const Rating>) { RECS_TODO("count_relevant"); }
double ndcg_at_k(std::span<const ItemIdx>, std::span<const Rating>, std::size_t) { RECS_TODO("ndcg_at_k"); }
double recall_at_k(std::span<const ItemIdx>, std::span<const Rating>, std::size_t) { RECS_TODO("recall_at_k"); }
double precision_at_k(std::span<const ItemIdx>, std::span<const Rating>, std::size_t) { RECS_TODO("precision_at_k"); }
double average_precision_at_k(std::span<const ItemIdx>, std::span<const Rating>, std::size_t) { RECS_TODO("average_precision_at_k"); }
double jaccard_at_k(std::span<const ItemIdx>, std::span<const ItemIdx>, std::size_t) { RECS_TODO("jaccard_at_k"); }
double catalog_coverage(const std::vector<std::vector<ItemIdx>>&, std::uint32_t, std::size_t) { RECS_TODO("catalog_coverage"); }
std::vector<std::uint32_t> popularity_ranks(const Csr&) { RECS_TODO("popularity_ranks"); }
double mean_popularity_rank(const std::vector<std::vector<ItemIdx>>&, const std::vector<std::uint32_t>&, std::size_t) { RECS_TODO("mean_popularity_rank"); }
double roc_auc(std::span<const float>, std::span<const ItemIdx>, std::span<const ItemIdx>) { RECS_TODO("roc_auc"); }
RatingErrors rating_errors(std::span<const float>, std::span<const Rating>) { RECS_TODO("rating_errors"); }
}  // namespace recs
