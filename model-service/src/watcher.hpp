// Watches data/artifacts/CURRENT and hot-swaps verified artifacts into a BundleHolder.
//
// Polling (every 2 s by default) is enough here: CURRENT changes at most once a night, a poll is
// one small file read, and polling has none of inotify's edge cases with rename-over and bind
// mounts. When CURRENT names a version other than the one serving, the watcher verifies and loads
// it off the request path (load_artifact checks every SHA-256), builds the Bundle, and swaps the
// shared_ptr. In-flight requests keep the snapshot they started with. If verification fails, the
// old bundle keeps serving, the failure is logged and counted, and that version is not retried
// (an artifact directory never changes after its atomic rename; a fixed artifact gets a new version).
//
// Workstream P (phase 5) owns the full retrain loop and may extend this (alerts, inotify).
#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "bundle.hpp"
#include "metrics.hpp"
#include "service.hpp"

namespace recs::service {

class ArtifactWatcher {
 public:
  enum class Poll { unchanged, swapped, failed, no_current };

  ArtifactWatcher(std::filesystem::path root, BundleHolder& holder, Metrics& metrics, Service& log_to);
  ~ArtifactWatcher();
  ArtifactWatcher(const ArtifactWatcher&) = delete;
  ArtifactWatcher& operator=(const ArtifactWatcher&) = delete;

  /// One check of CURRENT; loads and swaps if it names a new version. Never throws.
  Poll poll_once() noexcept;

  /// Polls every `interval` on a background thread until stop().
  void start(std::chrono::milliseconds interval = std::chrono::seconds(2));
  void stop();

 private:
  std::filesystem::path root_;
  BundleHolder& holder_;
  Metrics& metrics_;
  Service& log_;
  std::string failed_version_;  // last version that failed verification (not retried)
  bool reported_no_current_ = false;

  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace recs::service
