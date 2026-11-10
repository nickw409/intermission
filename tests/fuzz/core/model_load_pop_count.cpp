// libFuzzer: Model::load for "pop_count" on arbitrary model files (layout: model_load.hpp).
#include <cstddef>
#include <cstdint>

#include "model_load.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  return recs::fuzz::model_load("pop_count", data, size);
}
