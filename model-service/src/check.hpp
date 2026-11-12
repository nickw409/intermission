// recs-model --check: would this recs-model serve the artifact that CURRENT names?
//
// deploy/release.sh runs the image it is about to deploy with --check, against the artifacts
// directory production is using, before switching anything. A new recs-model can refuse an
// artifact that the running one serves: a model file format it no longer reads (for example MF
// format 1 after the min_support change), or, on a rollback, a newer format it does not know yet.
// Such a release needs a retrain first, and the check finds that out while the old version is
// still serving.
//
// The check runs the watcher's own poll_once(), the code the server uses at startup and for every
// hot swap, so it cannot disagree with what the server would do.
#pragma once

#include <filesystem>
#include <ostream>

#include "service.hpp"

namespace recs::service {

/// Loads and verifies the artifact root/CURRENT names. Returns 0 if the server would serve it and
/// writes, one per line, `version=`, `git_sha=` (the commit that trained it; "unknown" unless it
/// is a lowercase hex id of 7 to 40 characters) and `models=` (comma separated) to `out`.
/// Returns 1 otherwise; the reason goes to `log`, in the server's own log format.
int check_artifacts(const std::filesystem::path& root, std::ostream& out, LogSink log);

}  // namespace recs::service
