#!/usr/bin/env bash
# Monthly restore drill (spec: "A backup that hasn't been restored doesn't count"):
# restore the newest backup into a scratch compose project, start it on another loopback
# port, and run the smoke test against it. Production keeps running and is never written:
# the scratch project's database lives in a throwaway volume (deploy/compose.drill.yaml), and
# backups, reports and artifacts are mounted read-only.
#
#   deploy/restore-drill.sh            # images: the TAG in deploy/.env
#   deploy/restore-drill.sh <sha>      # images for another SHA
#
# Env: DRILL_PORT (default 18080); RECS_DRILL_PROJECT (default intermission-drill; set only by
# tests). Always tears the scratch project down; exits non-zero on any failure.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_env_file
# Wait up to 4 h: the drill timer can fire while a long nightly retrain is still running.
acquire_lock restore-drill 14400
require_data_dirs backups reports artifacts

tag="${1:-$(env_get TAG)}"
[[ -n "$tag" ]] || die "no TAG in $ENV_FILE and no SHA given"
tag="$(short_sha "$tag")"
images_present "$tag" || die "images for $tag are missing (deploy/build.sh $tag)"

# Overridable (RECS_DRILL_PROJECT) so a test of this script never tears down the real drill project.
project="${RECS_DRILL_PROJECT:-intermission-drill}"
[[ "$project" != intermission ]] || die "RECS_DRILL_PROJECT must not be the production project"
export TAG="$tag" RECS_HTTP_PORT="${DRILL_PORT:-18080}"
drill() {
  docker compose --project-name "$project" --project-directory "$DEPLOY_DIR" --env-file "$ENV_FILE" \
    -f "$COMPOSE_FILE" -f "$DEPLOY_DIR/compose.drill.yaml" "$@"
}

status=1
teardown() {
  drill down --volumes --remove-orphans >/dev/null 2>&1 || log "WARNING: drill teardown failed; run: docker compose -p $project down -v"
  job_status restore_drill "$status"
}
trap teardown EXIT

drill down --volumes --remove-orphans >/dev/null 2>&1 || true   # leftovers from a crashed drill

out="$(drill run --rm --no-deps backup latest --dir /srv/data/backups)" || die "no backup found"
latest="$(printf '%s' "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["latest"])')"
log "drill: restoring $latest (images $tag) into project $project"
drill run --rm --no-deps backup restore --from "$latest" --db /srv/data/db/app.db

drill up -d caddy api model
origin="$(env_get RECS_SITE_ORIGIN)"
wait_healthy "http://127.0.0.1:$RECS_HTTP_PORT/healthz" "$(origin_host "$origin")" 120 \
  || { drill logs --no-color --tail 50 >&2 || true; die "drill: /healthz not healthy within 120 s"; }
"$DEPLOY_DIR/smoke.sh" "http://127.0.0.1:$RECS_HTTP_PORT"

status=0
log "restore drill PASSED: $latest"
