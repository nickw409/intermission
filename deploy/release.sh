#!/usr/bin/env bash
# Deploy one image set (spec: "Build and deploy"). Rollback is the same command with an older SHA.
#
#   deploy/release.sh <sha>                  # deploy; on smoke failure, roll back automatically
#   deploy/release.sh --no-rollback <sha>    # deploy; on smoke failure, leave it running
#   deploy/release.sh --list                 # the last 5 deployed SHAs (deploy/history)
#
# Steps: make sure the images exist (build them from git if not), set TAG=<sha> in
# deploy/.env, `docker compose up -d`, wait for /healthz, run deploy/smoke.sh, then record the
# SHA in deploy/history (last 5 kept). Safe to re-run: deploying the running SHA again just
# re-checks it. Exits non-zero on any failure.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

HISTORY="$DEPLOY_DIR/history"
rollback=1
case "${1:-}" in
  --list) [[ -f "$HISTORY" ]] && cat "$HISTORY"; exit 0 ;;
  --no-rollback) rollback=0; shift ;;
esac
[[ $# -eq 1 ]] || die "usage: deploy/release.sh [--no-rollback] <sha> | --list"

require_env_file
command -v curl >/dev/null || die "curl is required"
require_data_dirs db exports posters reports artifacts

# One deploy at a time.
exec 9>"$DEPLOY_DIR/history.lock"
flock -n 9 || die "another release is in progress"

tag="$(short_sha "$1")"
previous="$(env_get TAG)"

if ! images_present "$tag"; then
  log "images for $tag not found locally; building them"
  "$DEPLOY_DIR/build.sh" "$1"
fi
images_present "$tag" || die "images for $tag are still missing"

# deploy runs inside `if`, where `set -e` is off, so every step checks its own status.
deploy() {
  local t="$1"
  log "deploying $t"
  env_set TAG "$t" || return 1
  compose up -d --remove-orphans caddy api model || { log "docker compose up failed"; return 1; }
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
    [[ -f "$HISTORY" ]] && grep -v "^$tag " "$HISTORY" || true
    printf '%s %s\n' "$tag" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } | tail -n 5 > "$HISTORY.tmp"
  mv -f "$HISTORY.tmp" "$HISTORY"
  log "release $tag OK (previous: ${previous:-none})"
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
