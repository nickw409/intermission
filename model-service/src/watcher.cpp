// CURRENT watcher and hot swap (see watcher.hpp).
#include "watcher.hpp"

namespace recs::service {

ArtifactWatcher::ArtifactWatcher(std::filesystem::path root, BundleHolder& holder, Metrics& metrics, Service& log_to)
    : root_(std::move(root)), holder_(holder), metrics_(metrics), log_(log_to) {}

ArtifactWatcher::~ArtifactWatcher() { stop(); }

ArtifactWatcher::Poll ArtifactWatcher::poll_once() noexcept {
  try {
    std::optional<std::string> version;
    try {
      version = read_current(root_);
    } catch (const std::exception& e) {
      log_.log_event("warn", std::string("cannot read CURRENT: ") + e.what());
      return Poll::failed;
    }
    if (!version) {
      if (!reported_no_current_) log_.log_event("warn", "no CURRENT pointer in " + root_.string());
      reported_no_current_ = true;
      return Poll::no_current;
    }
    reported_no_current_ = false;
    const auto serving = holder_.get();
    if ((serving && serving->version == *version) || *version == failed_version_) return Poll::unchanged;

    const auto t0 = std::chrono::steady_clock::now();
    try {
      auto bundle = make_bundle(load_artifact(version_dir(root_, *version)));
      if (bundle->version != *version) throw ArtifactError("manifest version does not match CURRENT");
      metrics_.set_artifact_load_seconds(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      holder_.store(std::move(bundle));
      log_.log_event("info", "serving artifact " + *version);
      return Poll::swapped;
    } catch (const std::exception& e) {
      failed_version_ = *version;
      metrics_.count_artifact_failure();
      log_.log_event("error", "artifact " + *version + " rejected, keeping " +
                                  (serving ? serving->version : std::string("none")) + ": " + e.what());
      return Poll::failed;
    }
  } catch (...) {
    return Poll::failed;
  }
}

void ArtifactWatcher::start(std::chrono::milliseconds interval) {
  stop();
  {
    std::lock_guard lock(mu_);
    stopping_ = false;
  }
  thread_ = std::thread([this, interval] {
    for (;;) {
      poll_once();
      std::unique_lock lock(mu_);
      if (cv_.wait_for(lock, interval, [this] { return stopping_; })) return;
    }
  });
}

void ArtifactWatcher::stop() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

}  // namespace recs::service
