#!/usr/bin/env bash
# Gates M2 (deploy and rollback; restore drill), using the owner's scripts unchanged:
#   1. images for SHA A and SHA B (deploy/build.sh, from clean checkouts of each commit);
#   2. a scratch "production": its own env file, data directory, compose project, lock file,
#      deploy history and loopback port; an artifact trained with A's train image; the smoke
#      account created through an invite (`recs-api admin invite create`) and signup through
#      Caddy, as in runbooks/deploy.md;
#   3. deploy/release.sh A, then B over A, then A again (rollback); each runs deploy/smoke.sh;
#   4. deploy/backup.sh, then deploy/restore-drill.sh into its own scratch drill project, which
#      restores the backup and runs the smoke test against it.
# Real operations are never touched: the scripts are pointed at scratch values through their
# overrides (RECS_ENV_FILE, RECS_LOCK_FILE, RECS_HISTORY_FILE, RECS_DRILL_PROJECT,
# COMPOSE_PROJECT_NAME, RECS_DATA_DIR), every other variable compose reads is scrubbed, and the
# project names of real stacks are refused. Everything it creates (containers, networks,
# volumes, scratch data) is removed on exit. Images are kept (remove with docker rmi).
#
#   tests/integration/run-deploy-drill.sh <sha-A> <sha-B>
#
#   DRILL_HTTP_PORT (default 18092) and DRILL_PORT (restore drill, default 18093)
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STACK_PORT="${DRILL_HTTP_PORT:-18092}"
drill_port="${DRILL_PORT:-18093}"
# shellcheck source=stack-lib.sh
source "$here/stack-lib.sh"

shaA="${1:?usage: run-deploy-drill.sh <sha-A> <sha-B>}"
shaB="${2:?usage: run-deploy-drill.sh <sha-A> <sha-B>}"
tagA="$(git -C "$STACK_REPO" rev-parse --short=7 "$shaA^{commit}")"
tagB="$(git -C "$STACK_REPO" rev-parse --short=7 "$shaB^{commit}")"
[[ "$tagA" != "$tagB" ]] || sdie "A and B must differ"

# Fixed scratch names, never inherited; stack_check_project refuses the real stacks' names.
STACK_PROJECT=intermission-m2
drill_project=intermission-m2-drill
stack_check_project
STACK_TAG="$tagA"
STACK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/intermission-m2.XXXXXX")"

cleanup() {
  set +e
  # The scratch drill project first (restore-drill.sh tears it down itself; this covers a
  # crash), then the scratch production project and data.
  docker compose --project-name "$drill_project" down --volumes --remove-orphans >/dev/null 2>&1
  stack_down
}
trap cleanup EXIT

t0=$SECONDS
step() { printf '\n== [%4ss] %s\n' "$((SECONDS - t0))" "$*"; }

step "build images for A=$tagA and B=$tagB (deploy/build.sh; existing images are reused)"
"$STACK_REPO/deploy/build.sh" "$tagA"
"$STACK_REPO/deploy/build.sh" "$tagB"

step "scratch production: data $STACK_DIR/data, project $STACK_PROJECT, 127.0.0.1:$STACK_PORT"
stack_init # scrubs the environment; exports RECS_DATA_DIR, RECS_HTTP_PORT and TAG (scratch)
# release.sh rewrites TAG in the env file, and a TAG in the shell would win over it.
unset TAG
# The deploy scripts call plain `docker compose -f deploy/compose.yaml` (project name from the
# file: intermission); point them, and the drill, at scratch everything.
export COMPOSE_PROJECT_NAME="$STACK_PROJECT" RECS_ENV_FILE="$STACK_DIR/env" \
  RECS_LOCK_FILE="$STACK_DIR/ops.lock" RECS_HISTORY_FILE="$STACK_DIR/history" \
  RECS_DRILL_PROJECT="$drill_project" DRILL_PORT="$drill_port"
sed -i "s/^TAG=.*/TAG=$tagA/" "$STACK_DIR/env"
# The smoke test asks for item-KNN, so the account below gets enough ratings for it.
sed -i 's/^SMOKE_MODEL=.*/SMOKE_MODEL=knn/' "$STACK_DIR/env"
stack_train ml-latest-small pop,pop_count,knn knn

step "first start with A, create the smoke account (runbooks/deploy.md step 6)"
TAG="$tagA" stack_up
code="$(TAG="$tagA" stack_invite 1)"
# Credentials never go through argv: password from the env file on stdin, request bodies and
# the session cookie in files inside a private 0700 directory (STACK_DIR itself is 0755).
cred="$(mktemp -d "$STACK_DIR/cred.XXXXXX")"
sed -n 's/^SMOKE_PASSWORD=//p' "$STACK_DIR/env" | stack_signup smoke-test "$code"
sed -n 's/^SMOKE_PASSWORD=//p' "$STACK_DIR/env" \
  | python3 -c 'import json,sys; print(json.dumps({"username": "smoke-test", "password": sys.stdin.read().rstrip("\n")}))' \
  > "$cred/login.json"
host_hdr="Host: ${STACK_ORIGIN#*://}"
curl -fsS -D "$cred/login.headers" -o /dev/null -X POST -H "$host_hdr" -H "Origin: $STACK_ORIGIN" \
  -H 'X-Requested-With: recs' -H 'Content-Type: application/json' --data-binary "@$cred/login.json" \
  "http://127.0.0.1:$STACK_PORT/api/auth/login"
sed -n 's/^[Ss]et-[Cc]ookie: *\(session=[^;]*\).*/Cookie: \1/p' "$cred/login.headers" | tr -d '\r' > "$cred/cookie"
rm -f "$cred/login.json" "$cred/login.headers"
[[ -s "$cred/cookie" ]] || sdie "smoke account login set no session cookie"
# A few ratings (movies the site has shown), so the smoke account's recommendations are personal.
curl -fsS -o /dev/null -H "$host_hdr" -H "@$cred/cookie" "http://127.0.0.1:$STACK_PORT/api/onboarding?n=30"
for m in 1 2 6 32 47 50 110 260 296 318; do
  curl -fsS -o /dev/null -X PUT -H "$host_hdr" -H "Origin: $STACK_ORIGIN" -H 'X-Requested-With: recs' \
    -H 'Content-Type: application/json' -H "@$cred/cookie" --data-binary '{"rating":4.5}' \
    "http://127.0.0.1:$STACK_PORT/api/ratings/$m"
done
rm -rf "$cred"

images() { local svc; for svc in caddy api model; do docker inspect --format "$svc {{.Config.Image}}" "$(stack_dc ps -q "$svc")"; done; }

step "deploy/release.sh A ($tagA): the running SHA, re-checked"
"$STACK_REPO/deploy/release.sh" "$tagA"

step "deploy/release.sh B ($tagB) over A"
s=$SECONDS; "$STACK_REPO/deploy/release.sh" "$tagB"; echo "release B took $((SECONDS - s)) s"
images

step "rollback: deploy/release.sh A ($tagA) over B"
s=$SECONDS; "$STACK_REPO/deploy/release.sh" "$tagA"; echo "rollback to A took $((SECONDS - s)) s"
images
"$STACK_REPO/deploy/release.sh" --list

step "deploy/backup.sh"
"$STACK_REPO/deploy/backup.sh"
stack_root find /s/data/backups -type f -printf '%P %s bytes %m\n'

step "deploy/restore-drill.sh (project $drill_project, 127.0.0.1:$drill_port)"
s=$SECONDS; "$STACK_REPO/deploy/restore-drill.sh"; echo "restore drill took $((SECONDS - s)) s"
cat "$STACK_DIR/data/ops/textfile/"*.prom

step "PASS: release A, B over A, rollback to A and the restore drill all passed the smoke test"
