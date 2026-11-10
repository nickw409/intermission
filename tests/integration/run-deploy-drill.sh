#!/usr/bin/env bash
# Gates M2 (deploy and rollback; restore drill), using the owner's scripts unchanged:
#   1. images for SHA A and SHA B (deploy/build.sh, from clean checkouts of each commit);
#   2. a scratch "production": its own env file, data directory, compose project and loopback
#      port; an artifact trained with A's train image; the smoke account created through an
#      invite (`recs-api admin invite create`) and signup through Caddy, as in runbooks/deploy.md;
#   3. deploy/release.sh A, then B over A, then A again (rollback); each runs deploy/smoke.sh;
#   4. deploy/backup.sh, then deploy/restore-drill.sh (restores into its own throwaway project
#      and runs the smoke test against it).
# Everything it creates (containers, networks, volumes, scratch data, deploy/history entries)
# is removed on exit. Images are kept (pass them to the other gates; remove with docker rmi).
#
#   tests/integration/run-deploy-drill.sh <sha-A> <sha-B>
#
#   DRILL_HTTP_PORT (default 18092) and DRILL_PORT (restore drill, default 18093)
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=stack-lib.sh
source "$here/stack-lib.sh"

shaA="${1:?usage: run-deploy-drill.sh <sha-A> <sha-B>}"
shaB="${2:?usage: run-deploy-drill.sh <sha-A> <sha-B>}"
tagA="$(git -C "$STACK_REPO" rev-parse --short=7 "$shaA^{commit}")"
tagB="$(git -C "$STACK_REPO" rev-parse --short=7 "$shaB^{commit}")"
[[ "$tagA" != "$tagB" ]] || { echo "A and B must differ" >&2; exit 2; }

# The production compose project name comes from compose.yaml (intermission); keep this drill
# off it, in case a real stack runs on this host.
export COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-intermission-m2}"
STACK_PROJECT="$COMPOSE_PROJECT_NAME"
STACK_PORT="${DRILL_HTTP_PORT:-18092}"
STACK_TAG="$tagA"
STACK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/intermission-m2.XXXXXX")"
export RECS_ENV_FILE="$STACK_DIR/env" RECS_LOCK_FILE="$STACK_DIR/ops.lock" DRILL_PORT="${DRILL_PORT:-18093}"

history="$STACK_REPO/deploy/history"
history_backup=""
if [[ -f "$history" ]]; then history_backup="$(mktemp)"; cp "$history" "$history_backup"; fi
cleanup() {
  stack_down
  if [[ -n "$history_backup" ]]; then mv -f "$history_backup" "$history"; else rm -f "$history"; fi
}
trap cleanup EXIT

t0=$SECONDS
step() { printf '\n== [%4ss] %s\n' "$((SECONDS - t0))" "$*"; }

step "build images for A=$tagA and B=$tagB (deploy/build.sh; existing images are reused)"
"$STACK_REPO/deploy/build.sh" "$tagA"
"$STACK_REPO/deploy/build.sh" "$tagB"

step "scratch production: data $STACK_DIR/data, project $STACK_PROJECT, 127.0.0.1:$STACK_PORT"
stack_init
# The smoke test asks for item-KNN, so the account below gets enough ratings for it.
sed -i 's/^SMOKE_MODEL=.*/SMOKE_MODEL=knn/' "$STACK_DIR/env"
stack_train ml-latest-small pop,pop_count,knn knn

step "first start with A, create the smoke account (runbooks/deploy.md step 6)"
stack_up
code="$(stack_invite 1)"
smoke_pass="$(sed -n 's/^SMOKE_PASSWORD=//p' "$STACK_DIR/env")"
stack_signup smoke-test "$smoke_pass" "$code"
# A few ratings (movies the site has shown: Toy Story, Heat, ...), so recommendations are personal.
login="$(python3 -c 'import json,sys; print(json.dumps({"username": "smoke-test", "password": sys.argv[1]}))' "$smoke_pass")"
cookie="$(curl -fsS -D - -o /dev/null -X POST -H "Host: ${STACK_ORIGIN#*://}" -H "Origin: $STACK_ORIGIN" \
  -H 'X-Requested-With: recs' -H 'Content-Type: application/json' --data-binary "$login" \
  "http://127.0.0.1:$STACK_PORT/api/auth/login" | sed -n 's/^[Ss]et-[Cc]ookie: *\(session=[^;]*\).*/\1/p' | tr -d '\r')"
curl -fsS -o /dev/null -H "Host: ${STACK_ORIGIN#*://}" -H "Cookie: $cookie" "http://127.0.0.1:$STACK_PORT/api/onboarding?n=30"
for m in 1 2 6 32 47 50 110 260 296 318; do
  curl -fsS -o /dev/null -X PUT -H "Host: ${STACK_ORIGIN#*://}" -H "Origin: $STACK_ORIGIN" -H 'X-Requested-With: recs' \
    -H 'Content-Type: application/json' -H "Cookie: $cookie" --data-binary '{"rating":4.5}' \
    "http://127.0.0.1:$STACK_PORT/api/ratings/$m"
done
unset cookie login smoke_pass

step "deploy/release.sh A ($tagA): the running SHA, re-checked"
"$STACK_REPO/deploy/release.sh" "$tagA"

step "deploy/release.sh B ($tagB) over A"
s=$SECONDS; "$STACK_REPO/deploy/release.sh" "$tagB"; echo "release B took $((SECONDS - s)) s"
for svc in caddy api model; do docker inspect --format "$svc {{.Config.Image}}" "$(stack_dc ps -q "$svc")"; done

step "rollback: deploy/release.sh A ($tagA) over B"
s=$SECONDS; "$STACK_REPO/deploy/release.sh" "$tagA"; echo "rollback to A took $((SECONDS - s)) s"
for svc in caddy api model; do docker inspect --format "$svc {{.Config.Image}}" "$(stack_dc ps -q "$svc")"; done
"$STACK_REPO/deploy/release.sh" --list

step "deploy/backup.sh"
"$STACK_REPO/deploy/backup.sh"
stack_root find /s/data/backups -type f -printf '%P %s bytes %m\n'

step "deploy/restore-drill.sh (project intermission-drill, 127.0.0.1:$DRILL_PORT)"
s=$SECONDS; "$STACK_REPO/deploy/restore-drill.sh"; echo "restore drill took $((SECONDS - s)) s"
cat "$STACK_DIR/data/ops/textfile/"*.prom

step "PASS: release A, B over A, rollback to A and the restore drill all passed the smoke test"
