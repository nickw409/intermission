// recs-model --check (see check.hpp).
#include "check.hpp"

#include <string>
#include <string_view>

#include "bundle.hpp"
#include "metrics.hpp"
#include "watcher.hpp"

namespace recs::service {

namespace {

// The commit id as the deploy script may read it: a short or full lowercase hex SHA, else
// "unknown". The manifest is data, so nothing else from it reaches the output.
std::string safe_git_sha(std::string_view sha) {
  if (sha.size() < 7 || sha.size() > 40) return "unknown";
  for (const char c : sha) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "unknown";
  }
  return std::string(sha);
}

}  // namespace

int check_artifacts(const std::filesystem::path& root, std::ostream& out, LogSink log) {
  BundleHolder holder;
  Metrics metrics;
  Service service(holder, metrics, std::move(log));
  ArtifactWatcher watcher(root, holder, metrics, service);
  switch (watcher.poll_once()) {  // the watcher logs why whenever it does not swap
    case ArtifactWatcher::Poll::swapped:
      break;
    case ArtifactWatcher::Poll::no_current:
      return kCheckNoCurrent;
    default:
      return kCheckRefused;
  }

  const auto bundle = holder.get();
  std::string models;
  for (const auto& name : bundle->manifest.models) {
    if (!models.empty()) models += ',';
    models += name;
  }
  out << "version=" << bundle->version << '\n'
      << "git_sha=" << safe_git_sha(bundle->manifest.git_sha) << '\n'
      << "models=" << models << '\n';
  return out ? kCheckOk : kCheckRefused;
}

}  // namespace recs::service
