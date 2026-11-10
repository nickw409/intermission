// STUB (Wave 0). Owner: workstream F. Replace with the real implementation.
#include "models/mf_sgd_detail.hpp"
#include "recs/model.hpp"
#include "util/todo.hpp"
namespace recs {
namespace mf_detail {
double predict(const Params&, std::uint32_t, std::uint32_t) { RECS_TODO("mf predict"); }
double objective(const Params&, std::span<const Triplet>, double) { RECS_TODO("mf objective"); }
Params gradient(const Params&, std::span<const Triplet>, double) { RECS_TODO("mf gradient"); }
void sgd_step(Params&, const Triplet&, double, double) { RECS_TODO("mf sgd_step"); }
}  // namespace mf_detail
std::unique_ptr<Model> make_mf_sgd() { RECS_TODO("MF-SGD model"); }
}  // namespace recs
