#!/usr/bin/env bash
# Nightly batch work (spec: "The retrain loop"), run by deploy/systemd/intermission-nightly.timer:
#   1. recs-api export-ratings   website ratings -> data/exports/ (internal user ids only)
#   2. recs-train retrain        merge export + MovieLens, train, eval, publish a new artifact;
#                                recs-model hot-swaps it when CURRENT changes
#   3. backup                    deploy/backup.sh
#
# Retrain only runs after a successful export (it would otherwise train on a stale export).
#
# RECS_NIGHTLY_RETRAIN (deploy/.env): `local` (default) runs step 2 here. `off` is for a host that
# only serves (the Raspberry Pi: too little memory to train; training runs on another machine,
# docs/plans/2026-11-12-pi-migration.md §4). Step 2 is then skipped, and any retrain status file
# left from an earlier run is removed so an old failure cannot keep JobFailed firing.
# The backup always runs. Each step's result is written for the JobFailed /
# NightlyJobStale alerts. Exits non-zero if any step failed.
#
# Priority: the train container has a low CPU weight (cpu_shares) and a raised OOM score in
# compose.yaml; the systemd unit's Nice/IOSchedulingClass only affect the docker client.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_env_file
retrain_mode="$(env_get RECS_NIGHTLY_RETRAIN)"
retrain_mode="${retrain_mode:-local}"
case "$retrain_mode" in
  local|off) ;;
  *) die "RECS_NIGHTLY_RETRAIN must be local or off, not '$retrain_mode'" ;;
esac
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

if [[ "$retrain_mode" == off ]]; then
  step export compose run --rm --no-deps api export-ratings || true
  log "nightly: retrain: off on this host (RECS_NIGHTLY_RETRAIN=off); training runs elsewhere"
  rm -f "$(data_dir)/ops/textfile/intermission_job_retrain.prom" \
    || log "WARNING: could not remove the old retrain status file (check data/ops/textfile ownership)"
elif step export compose run --rm --no-deps api export-ratings; then
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
