#!/usr/bin/env bash
# Deploy one image set (spec: "Build and deploy"). Rollback is the same command with an older SHA.
#
#   deploy/release.sh <sha>                  # deploy; on smoke failure, roll back automatically
#   deploy/release.sh --no-rollback <sha>    # deploy; on smoke failure, leave it running
#   deploy/release.sh --list                 # the last 5 deployed SHAs (deploy/history;
#                                            # RECS_HISTORY_FILE overrides it, tests only)
#
# Steps: make sure the images exist (build them from git if not), check that the new recs-model
# loads the current model artifact (stop, changing nothing, if not: a retrain is needed), set TAG=<sha> in
# deploy/.env, `docker compose up -d`, wait for /healthz, run deploy/smoke.sh, then record the
# SHA in deploy/history (last 5 kept). Safe to re-run: deploying the running SHA again just
# re-checks it. Exits non-zero on any failure.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

HISTORY="$HISTORY_FILE"
rollback=1
case "${1:-}" in
  --list) [[ -f "$HISTORY" ]] && cat "$HISTORY"; exit 0 ;;
  --no-rollback) rollback=0; shift ;;
esac
[[ $# -eq 1 ]] || die "usage: deploy/release.sh [--no-rollback] <sha> | --list"

require_env_file
command -v curl >/dev/null || die "curl is required"
require_data_dirs db exports posters reports artifacts

# One operation at a time (shared with backup, nightly and restore-drill). A deploy waits at
# most 2 minutes rather than queueing behind a long retrain.
acquire_lock release "${RELEASE_LOCK_WAIT:-120}"

tag="$(short_sha "$1")"
previous="$(env_get TAG)"

if ! images_present "$tag"; then
  log "images for $tag not found locally; building them"
  "$DEPLOY_DIR/build.sh" "$1"
fi
images_present "$tag" || die "images for $tag are still missing"

# Preflight, before anything changes: the new recs-model must accept the artifact CURRENT points
# at. It refuses one whose model file format it no longer reads (or, on a rollback, a newer format
# it does not know yet); that release needs a retrain with its own train image first. The running
# version keeps serving either way.
log "checking that $tag's recs-model can load the current model artifact"
if ! check_out="$(check_artifact "$tag")"; then
  job_status artifact_check 1
  die "release $tag stopped before any change: its recs-model cannot load the current model artifact (reason above). Retrain with the $tag train image first: deploy/runbooks/deploy.md, \"Model artifacts that need a retrain first\""
fi
artifact_version="$(sed -n 's/^version=//p' <<<"$check_out")"
artifact_sha="$(sed -n 's/^git_sha=//p' <<<"$check_out")"
if [[ -n "$artifact_version" ]]; then  # empty: the image predates --check (skipped, see above)
  job_status artifact_check 0
  log "artifact $artifact_version (trained at ${artifact_sha:-unknown}) loads with $tag"
fi

# deploy runs inside `if`, where `set -e` is off, so every step checks its own status.
deploy() {
  local t="$1"
  log "deploying $t"
  env_set TAG "$t" || return 1
  compose up -d caddy api model || { log "docker compose up failed"; return 1; }
  # Every service must now run the new image (guards against a half-applied `up`).
  local svc img
  for svc in caddy api model; do
    img="$(docker inspect --format '{{.Config.Image}}' "$(compose ps -q "$svc")" 2>/dev/null)" || img=""
    [[ "$img" == *":$t" ]] || { log "$svc is running '${img:-nothing}', expected tag $t"; return 1; }
  done
  local origin port
  origin="$(env_get RECS_SITE_ORIGIN)"
  port="${RECS_HTTP_PORT:-$(env_get RECS_HTTP_PORT)}"
  wait_healthy "http://127.0.0.1:${port:-8080}/healthz" "$(origin_host "$origin")" 120 \
    || { log "/healthz did not become healthy within 120 s"; return 1; }
  "$DEPLOY_DIR/smoke.sh"
}

if deploy "$tag"; then
  {
    if [[ -f "$HISTORY" ]]; then grep -v "^$tag " "$HISTORY" || true; fi  # grep: 1 = no other lines
    printf '%s %s\n' "$tag" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } | tail -n 5 > "$HISTORY.tmp"
  mv -f "$HISTORY.tmp" "$HISTORY"
  log "release $tag OK (previous: ${previous:-none})"
  # The artifact loads, but was it trained by older training code? Then the site serves a model
  # the code has moved past (a new feature such as KNN's mean_damping only arrives with a retrain).
  # Recorded only now, against the version that is actually running.
  stale=0
  training_changed "$artifact_sha" "$tag" || stale=$?
  case "$stale" in
    0) log "NOTE: core/, train/ or configs/ changed since artifact $artifact_version was trained at $artifact_sha; a retrain is recommended"
       job_status artifact_freshness 1 ;;
    1) job_status artifact_freshness 0 ;;
    *) log "NOTE: cannot tell whether artifact $artifact_version is up to date (trained at '${artifact_sha:-unknown}', not a commit in this repository)" ;;
  esac
  exit 0
fi

log "release $tag FAILED"
compose logs --no-color --tail 50 api model caddy >&2 || true
if [[ $rollback -eq 1 && -n "$previous" && "$previous" != "$tag" ]] && images_present "$previous"; then
  log "rolling back to $previous"
  if deploy "$previous"; then
    log "rolled back to $previous; $tag is NOT deployed"
  else
    log "ROLLBACK TO $previous ALSO FAILED: the site is degraded. See deploy/runbooks/rollback.md"
  fi
else
  log "no automatic rollback (previous: ${previous:-none}); see deploy/runbooks/rollback.md"
fi
exit 1
