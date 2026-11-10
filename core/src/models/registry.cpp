// The only list of models. Spine-owned wiring file: a new model adds one line here (orchestrator).
#include <stdexcept>
#include <string>

#include "recs/model.hpp"

namespace recs {

std::unique_ptr<Model> make_model(std::string_view name) {
  if (name == "pop") return make_popularity();
  if (name == "pop_count") return make_popularity_count();
  if (name == "knn") return make_item_knn();
  if (name == "content") return make_content();
  if (name == "mf") return make_mf_sgd();
  if (name == "als") return make_als();
  throw std::invalid_argument("unknown model: " + std::string(name));
}

std::vector<std::string> model_names() { return {"pop", "pop_count", "knn", "content", "mf", "als"}; }

}  // namespace recs
