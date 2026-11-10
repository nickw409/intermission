// The recs-model router and request handlers (contracts/model.yaml).
//
// Service::handle() is the single entry point for every request. It never throws: input problems
// become 4xx responses with the contract's Error shape, and any unexpected exception becomes a 500
// `internal`. It logs one JSON line per request (request id, method, route template, status,
// latency, model version) and never logs bodies, query strings or rating payloads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "bundle.hpp"
#include "http_types.hpp"
#include "metrics.hpp"

namespace recs::service {

/// Input limits from contracts/model.yaml.
struct Limits {
  static constexpr std::size_t kMaxBodyBytes = 1u << 20;  // 1 MiB
  static constexpr std::size_t kMaxRatings = 5000;
  static constexpr std::size_t kMaxExclude = 10000;
  static constexpr std::size_t kMaxK = 100;
  static constexpr std::size_t kMaxBatchIds = 200;
  static constexpr std::size_t kMaxCompareModels = 5;
  static constexpr std::size_t kMaxQueryChars = 100;
  static constexpr std::size_t kMaxSearchLimit = 50;
  static constexpr std::size_t kDefaultSearchLimit = 20;
  static constexpr std::size_t kDefaultSimilarK = 20;
  static constexpr std::int64_t kMaxMovieId = 2147483647;
  static constexpr int kMaxJsonDepth = 8;  // deepest valid request is 3
};

/// Receives one complete log line (a JSON object, no trailing newline).
using LogSink = std::function<void(std::string_view line)>;

/// Writes log lines to stderr, one per line, serialized by a mutex.
LogSink stderr_log_sink();

class Service {
 public:
  Service(const BundleHolder& holder, Metrics& metrics, LogSink log);

  /// Routes and answers one request. Never throws.
  Response handle(const Request& req) noexcept;

  /// The response for an error the HTTP layer detected before routing (oversized body, malformed
  /// request line, ...). `http_status` is the status the HTTP layer chose. Never throws.
  Response transport_error(int http_status, std::string_view request_id) noexcept;

  /// A contract Error response (`code`, `message`) for a request refused before routing, e.g. by
  /// the HTTP layer's framing checks. Logged and counted under route "other". Never throws.
  Response error_response(int status, std::string_view code, std::string_view message,
                          std::string_view request_id) noexcept;

  /// Writes a log line through the configured sink (used by the HTTP layer and main()).
  void log_event(std::string_view level, std::string_view msg) noexcept;

 private:
  const BundleHolder& holder_;
  Metrics& metrics_;
  LogSink log_;
};

}  // namespace recs::service
