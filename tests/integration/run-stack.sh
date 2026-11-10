#!/usr/bin/env bash
# Gates N2 (robustness end to end through Caddy) and N3 (degradation) against the real compose
# stack on 127.0.0.1, plus the compose port check (no published port except 127.0.0.1:8080).
#
#   tests/integration/run-stack.sh <tag>        # images intermission-*:<tag> (deploy/build.sh)
#
# Starts a scratch compose project (intermission-it, 127.0.0.1:${STACK_PORT:-18090}) with its
# own data directory, trains a small artifact on ml-latest-small with the train image, creates
# an invite with `recs-api admin invite create`, runs tests/integration/stack, and tears
# everything down (containers, networks, volumes, scratch data) on exit.
#   KEEP_STACK=1   leave it running for debugging (tear down later with the printed command)
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=stack-lib.sh
source "$here/stack-lib.sh"

STACK_TAG="${1:?usage: run-stack.sh <tag>}"
STACK_PROJECT=intermission-it # fixed: never inherited (stack_check_project refuses real stacks)
stack_check_project
STACK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/intermission-it.XXXXXX")"
if [[ -z "${KEEP_STACK:-}" ]]; then
  trap stack_down EXIT
else
  trap 'slog "kept; tear down with: STACK_PROJECT=$STACK_PROJECT STACK_DIR=$STACK_DIR bash -c \"source $here/stack-lib.sh; stack_down\""' EXIT
fi

stack_init # scrubs the environment and exports the scratch values

echo "== compose config: published ports (production project, clean environment)"
(unset RECS_DATA_DIR RECS_HTTP_PORT TAG; python3 "$STACK_REPO/deploy/tests/check_compose.py")

stack_train ml-latest-small pop,pop_count,knn,content,mf,als knn
stack_up
code="$(stack_invite 50)"
[[ -n "$code" ]] || { slog "no invite code"; exit 1; }

q() { printf '%q ' "$@"; }
dc="$(q docker compose --project-name "$STACK_PROJECT" --project-directory "$STACK_REPO/deploy" --env-file "$STACK_DIR/env" -f "$STACK_REPO/deploy/compose.yaml")"
export RECS_STACK_URL="http://127.0.0.1:$STACK_PORT" RECS_STACK_ORIGIN="$STACK_ORIGIN" RECS_STACK_INVITE="$code"
export RECS_STACK_STOP_MODEL="$dc stop model" RECS_STACK_START_MODEL="$dc start model"
export RECS_STACK_PAUSE_MODEL="$dc pause model" RECS_STACK_UNPAUSE_MODEL="$dc unpause model"

echo "== N2 robustness and N3 degradation through Caddy (127.0.0.1:$STACK_PORT)"
cd "$here"
go test -count=1 -v ./stack 2>&1 | grep -v -e '^=== RUN' -e '^=== PAUSE' -e '^=== CONT'
