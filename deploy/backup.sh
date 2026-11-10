#!/usr/bin/env bash
# Back up app.db (spec: "Backups and recovery"): VACUUM INTO a new file, PRAGMA integrity_check,
# 0600 permissions, keep 7 daily + 4 weekly copies under data/backups/{daily,weekly}/.
# Runs the one-shot `backup` compose service (distroless recs-backup, UID 10001, no network).
# Safe to re-run: a second run on the same day replaces that day's copy.
#
#   deploy/backup.sh
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_env_file
acquire_lock backup
require_data_dirs db backups

status=0
compose run --rm --no-deps backup || status=$?
job_status backup "$status"
[[ $status -eq 0 ]] || die "backup failed (exit $status)"
log "backup OK"
