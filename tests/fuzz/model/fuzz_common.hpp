// Shared setup for the recs-model libFuzzer harnesses: one Service over the in-memory fixture
// artifact (tests/model-service/test_bundle.hpp), and the invariants every response must satisfy.
// A violated invariant traps, so libFuzzer reports it like a crash.
#pragma once

#include <set>
#include <string_view>

#include <nlohmann/json.hpp>

#include "../../model-service/test_bundle.hpp"
#include "service.hpp"

namespace recs::fuzz {

struct Harness {
  service::BundleHolder holder;
  service::Metrics metrics;
  service::Service svc{holder, metrics, [](std::string_view) {}};
  Harness() { holder.store(testing::fixture_bundle()); }
};

inline Harness& harness() {
  static Harness h;
  return h;
}

inline void check_response(const service::Response& r) {
  static const std::set<int> allowed = {200, 400, 404, 405, 503};
  // Every input problem must be a 4xx; a 500 `internal` means a handler threw unexpectedly.
  if (!allowed.contains(r.status)) __builtin_trap();
  if (r.model_version.empty() || r.request_id.empty()) __builtin_trap();
  if (r.content_type == "application/json") {
    const auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) __builtin_trap();
    if (r.status >= 400 && !(j.contains("error") || j.contains("status"))) __builtin_trap();
  }
}

}  // namespace recs::fuzz
