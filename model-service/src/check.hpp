// recs-model --check: would this recs-model serve the artifact that CURRENT names?
//
// deploy/release.sh runs the image it is about to deploy with --check, against the artifacts
// directory production is using, before switching anything. A new recs-model can refuse an
// artifact that the running one serves: a model file format it no longer reads (for example MF
// format 1 after the min_support change), or, on a rollback, a newer format it does not know yet.
// Such a release needs a retrain first, and the check finds that out while the old version is
// still serving. Only images that have this flag can be asked: release.sh skips the check for a
// rollback to an image built before it.
//
// The check runs the watcher's own poll_once(), the code the server uses at startup and for every
// hot swap, so it cannot disagree with what the server would do.
#pragma once

#include <filesystem>
#include <ostream>

#include "service.hpp"

namespace recs::service {

/// Exit codes of recs-model --check. Distinct from 1 and from CLI11's parse errors, so the deploy
/// script can tell "this artifact needs a retrain" from a failure to run the check at all.
inline constexpr int kCheckOk = 0;
inline constexpr int kCheckRefused = 3;    // CURRENT names an artifact the server would not serve
inline constexpr int kCheckNoCurrent = 4;  // there is no CURRENT pointer (nothing to check)

/// Loads and verifies the artifact root/CURRENT names. Returns kCheckOk if the server would serve
/// it and writes, one per line, `version=`, `git_sha=` (the commit that trained it; "unknown"
/// unless it is a lowercase hex id of 7 to 40 characters) and `models=` (comma separated) to
/// `out`. Otherwise returns kCheckRefused or kCheckNoCurrent, writes nothing to `out`, and the
/// reason goes to `log`, in the server's own log format.
int check_artifacts(const std::filesystem::path& root, std::ostream& out, LogSink log);

}  // namespace recs::service
