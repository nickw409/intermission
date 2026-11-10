// libFuzzer: arbitrary bytes as the JSON body of each POST route. The first byte picks the route.
#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  static const char* const kRoutes[] = {"/v1/recommend", "/v1/compare", "/v1/onboarding", "/v1/movies/batch"};
  recs::service::Request req;
  req.method = "POST";
  req.path = kRoutes[data[0] % 4];
  req.body.assign(reinterpret_cast<const char*>(data + 1), size - 1);
  recs::fuzz::check_response(recs::fuzz::harness().svc.handle(req));
  return 0;
}
