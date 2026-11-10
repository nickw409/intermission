// Prometheus metrics for recs-model (text exposition format 0.0.4).
//
// Label values come only from fixed sets (route templates, status codes this service emits, model
// names from the contract enum), never from request content, so the number of series is bounded no
// matter what callers send.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recs::service {

class Histogram {
 public:
  static constexpr std::array<double, 13> kBuckets = {0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01,
                                                      0.025,  0.05,    0.1,    0.25,  0.5,   1.0};
  void observe(double seconds);
  /// Appends the _bucket/_sum/_count lines for `name` with the given label set ("" for none).
  void render(std::string& out, std::string_view name, std::string_view labels) const;

 private:
  std::array<std::uint64_t, kBuckets.size()> counts_{};  // non-cumulative
  std::uint64_t count_ = 0;
  double sum_ = 0.0;
};

class Metrics {
 public:
  /// One finished request. `route` is a route template such as "/v1/similar/{movie_id}".
  void observe_request(std::string_view route, int status, double seconds);
  /// One fold-in (fold_in() + scoring + top-k) for `model`.
  void observe_foldin(std::string_view model, double seconds);
  /// Time taken to verify and load the artifact now serving.
  void set_artifact_load_seconds(double seconds);

  /// The full exposition. `version` is the serving artifact version ("none" if none).
  std::string render(std::string_view version) const;

 private:
  mutable std::mutex mu_;
  std::map<std::pair<std::string, int>, std::uint64_t> requests_;
  std::map<std::string, Histogram> latency_;
  std::map<std::string, Histogram> foldin_;
  double artifact_load_seconds_ = 0.0;
};

}  // namespace recs::service
