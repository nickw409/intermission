// STUB (Wave 0). Owner: workstream D. Replace with the real implementation.
#include "models/item_knn_detail.hpp"
#include "recs/model.hpp"
#include "util/todo.hpp"
namespace recs {
std::unique_ptr<Model> make_item_knn() { RECS_TODO("item-KNN model"); }
namespace item_knn_detail {
std::vector<std::vector<PairSimilarity>> full_similarity(const Dataset&, const Config&) { RECS_TODO("knn"); }
std::vector<Scored> neighbour_list(const Model&, ItemIdx) { RECS_TODO("knn"); }
}  // namespace item_knn_detail
}  // namespace recs
