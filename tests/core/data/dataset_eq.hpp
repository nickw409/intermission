// Field-by-field Dataset comparison for the data tests (Dataset itself has no operator==).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include "recs/types.hpp"

namespace recs::testing {

inline void check_id_map_equal(const IdMap& a, const IdMap& b) {
  CHECK(a.to_external == b.to_external);
  REQUIRE(a.to_dense.size() == b.to_dense.size());
  for (const auto& [ext, dense] : a.to_dense) CHECK(b.find(ext) == dense);
}

inline void check_dataset_equal(const Dataset& a, const Dataset& b) {
  CHECK(a.name == b.name);
  check_id_map_equal(a.users, b.users);
  check_id_map_equal(a.items, b.items);
  CHECK(a.by_user == b.by_user);
  CHECK(a.by_item == b.by_item);
  CHECK(a.movies == b.movies);
  CHECK(a.tags == b.tags);
  CHECK(a.genome == b.genome);
}

}  // namespace recs::testing
