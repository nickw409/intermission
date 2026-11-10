#!/usr/bin/env bash
# Nightly batch work (spec: "The retrain loop"), run by deploy/systemd/intermission-nightly.timer:
#   1. recs-api export-ratings   website ratings -> data/exports/ (internal user ids only)
#   2. recs-train retrain        merge export + MovieLens, train, eval, publish a new artifact;
#                                recs-model hot-swaps it when CURRENT changes
#   3. backup                    deploy/backup.sh
#
# Retrain only runs after a successful export (it would otherwise train on a stale export).
# The backup always runs. Each step's result is written for the JobFailed /
# NightlyJobStale alerts. Exits non-zero if any step failed.
#
# Priority: the train container has a low CPU weight (cpu_shares) and a raised OOM score in
# compose.yaml; the systemd unit's Nice/IOSchedulingClass only affect the docker client.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_env_file
acquire_lock nightly
require_data_dirs exports artifacts reports reports-private cache raw db backups

failed=()

step() { # step NAME COMMAND...
  local name="$1"; shift
  local status=0
  log "nightly: $name: start"
  "$@" || status=$?
  job_status "$name" "$status"
  if [[ $status -eq 0 ]]; then
    log "nightly: $name: OK"
  else
    log "nightly: $name: FAILED (exit $status)"
    failed+=("$name")
  fi
  return "$status"
}

if step export compose run --rm --no-deps api export-ratings; then
  step retrain compose run --rm --no-deps train retrain || true
else
  log "nightly: skipping retrain because export failed"
  job_status retrain 1
  failed+=(retrain)
fi
step backup "$DEPLOY_DIR/backup.sh" || true

if [[ ${#failed[@]} -gt 0 ]]; then
  die "nightly: failed steps: ${failed[*]}"
fi
log "nightly: all steps OK"
