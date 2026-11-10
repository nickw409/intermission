// Prometheus text exposition (https://prometheus.io/docs/instrumenting/exposition_formats/).
#include "metrics.hpp"

#include <charconv>
#include <cmath>

namespace recs::service {
namespace {

std::string fmt(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

// Label values are from fixed sets, but escape anyway (backslash, quote, newline).
std::string esc(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

void Histogram::observe(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
  for (std::size_t b = 0; b < kBuckets.size(); ++b) {
    if (seconds <= kBuckets[b]) {
      ++counts_[b];
      break;
    }
  }
  ++count_;
  sum_ += seconds;
}

void Histogram::render(std::string& out, std::string_view name, std::string_view labels) const {
  const std::string sep = labels.empty() ? "" : ",";
  std::uint64_t cumulative = 0;
  for (std::size_t b = 0; b < kBuckets.size(); ++b) {
    cumulative += counts_[b];
    out += std::string(name) + "_bucket{" + std::string(labels) + sep + "le=\"" + fmt(kBuckets[b]) + "\"} " +
           std::to_string(cumulative) + "\n";
  }
  out += std::string(name) + "_bucket{" + std::string(labels) + sep + "le=\"+Inf\"} " + std::to_string(count_) + "\n";
  const std::string braces = labels.empty() ? "" : "{" + std::string(labels) + "}";
  out += std::string(name) + "_sum" + braces + " " + fmt(sum_) + "\n";
  out += std::string(name) + "_count" + braces + " " + std::to_string(count_) + "\n";
}

void Metrics::observe_request(std::string_view route, int status, double seconds) {
  std::lock_guard lock(mu_);
  ++requests_[{std::string(route), status}];
  latency_[std::string(route)].observe(seconds);
}

void Metrics::observe_foldin(std::string_view model, double seconds) {
  std::lock_guard lock(mu_);
  foldin_[std::string(model)].observe(seconds);
}

void Metrics::set_artifact_load_seconds(double seconds) {
  std::lock_guard lock(mu_);
  artifact_load_seconds_ = seconds;
}

std::string Metrics::render(std::string_view version) const {
  std::lock_guard lock(mu_);
  std::string out;
  out += "# HELP recs_model_requests_total Requests by route template and status code.\n";
  out += "# TYPE recs_model_requests_total counter\n";
  for (const auto& [key, n] : requests_) {
    out += "recs_model_requests_total{route=\"" + esc(key.first) + "\",status=\"" + std::to_string(key.second) + "\"} " +
           std::to_string(n) + "\n";
  }
  out += "# HELP recs_model_request_duration_seconds Request latency by route template.\n";
  out += "# TYPE recs_model_request_duration_seconds histogram\n";
  for (const auto& [route, h] : latency_) h.render(out, "recs_model_request_duration_seconds", "route=\"" + esc(route) + "\"");
  out += "# HELP recs_model_foldin_duration_seconds Fold-in, scoring and top-k time per model.\n";
  out += "# TYPE recs_model_foldin_duration_seconds histogram\n";
  for (const auto& [model, h] : foldin_) h.render(out, "recs_model_foldin_duration_seconds", "model=\"" + esc(model) + "\"");
  out += "# HELP recs_model_artifact_loaded 1 if a verified artifact is serving.\n";
  out += "# TYPE recs_model_artifact_loaded gauge\n";
  out += std::string("recs_model_artifact_loaded ") + (version == "none" ? "0" : "1") + "\n";
  out += "# HELP recs_model_artifact_info The artifact version serving (value is always 1).\n";
  out += "# TYPE recs_model_artifact_info gauge\n";
  out += "recs_model_artifact_info{version=\"" + esc(version) + "\"} 1\n";
  out += "# HELP recs_model_artifact_load_seconds Time to verify and load the serving artifact.\n";
  out += "# TYPE recs_model_artifact_load_seconds gauge\n";
  out += "recs_model_artifact_load_seconds " + fmt(artifact_load_seconds_) + "\n";
  return out;
}

}  // namespace recs::service
