#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "recs/model.hpp"

TEST_CASE("Registry lists every model and rejects unknown names", "[spine][registry]") {
  CHECK(recs::model_names() == std::vector<std::string>{"pop", "pop_count", "knn", "content", "mf", "als"});
  CHECK_THROWS_AS(recs::make_model("nope"), std::invalid_argument);
}
