# Helpers for running a scratch Intermission compose stack on 127.0.0.1 (integration and load
# gates). Source it; don't run it. Uses the production compose file and images exactly as the
# deploy scripts do, with:
#   - its own compose project name and loopback port, so it never touches another stack;
#   - its own data directory (STACK_DIR/data) and env file (STACK_DIR/env);
#   - MovieLens mounted read-only from RAW_DIR (never written).
#
# Safety: docker compose gives variables from the shell precedence over --env-file, so a stray
# `export RECS_DATA_DIR=/srv/data` or `TAG=...` would point a scratch stack at production data
# or the wrong images. stack_init therefore scrubs every variable compose or the deploy
# scripts read and exports the scratch values (stack_scrub_env). Project names that belong to
# real stacks are refused.
#
# The data directories need the owners the containers run as (deploy/init-data.sh, which wants
# root). Without sudo, the helpers run init-data.sh, and the final cleanup, in a throwaway root
# container (Docker access is root-equivalent anyway).
#
# Set by the calling script (never inherited from the environment):
#   STACK_PROJECT   compose project name (not intermission, intermission-drill, intermission-dev)
#   STACK_DIR       scratch directory (created by the caller)
#   STACK_TAG       image tag (short SHA)
# Optional:
#   STACK_PORT      loopback port for caddy          (default 18090)
#   RAW_DIR         MovieLens root (holds ml-latest-small/, ml-32m/)  (default: repo data/raw)
# shellcheck shell=bash

STACK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STACK_PORT="${STACK_PORT:-18090}"
RAW_DIR="${RAW_DIR:-$STACK_REPO/data/raw}"
STACK_ORIGIN="http://intermission.test"
# A root-capable image with a shell, already pulled by deploy/build.sh (the C++ builder base).
STACK_HELPER_IMAGE="ubuntu:noble-20261024.1@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852"

slog() { printf '%s [stack] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
sdie() { slog "ERROR: $*"; exit 1; }

# stack_check_project: refuse the names of real stacks.
stack_check_project() {
  [[ -n "${STACK_PROJECT:-}" ]] || sdie "STACK_PROJECT is not set"
  case "$STACK_PROJECT" in
    intermission | intermission-drill | intermission-dev)
      sdie "refusing to use project name '$STACK_PROJECT': it belongs to a real stack" ;;
  esac
}

# stack_scrub_env: drop every variable compose and the deploy scripts would read from the
# shell, then export the scratch values. Called by stack_init; safe to call again.
stack_scrub_env() {
  unset COMPOSE_PROJECT_NAME COMPOSE_FILE COMPOSE_PROFILES COMPOSE_ENV_FILES COMPOSE_PATH_SEPARATOR \
    RECS_ARTIFACTS_DIR RECS_DEV_DATA_DIR RECS_DEV_HTTP_PORT RECS_SITE_ORIGIN RECS_SIGNUP_MODE \
    RECS_LOG_LEVEL TMDB_API_KEY SMOKE_USERNAME SMOKE_PASSWORD SMOKE_MODEL GRAFANA_ADMIN_PASSWORD \
    RECS_ENV_FILE RECS_LOCK_FILE RECS_HISTORY_FILE RECS_DRILL_PROJECT DRILL_PORT INTERMISSION_LOCK_HELD \
    NODE_EXPORTER_DATA_PROPAGATION MODEL_MEM_LIMIT MODEL_OMP_THREADS TRAIN_MEM_LIMIT TRAIN_OMP_THREADS
  export RECS_DATA_DIR="$STACK_DIR/data" RECS_HTTP_PORT="$STACK_PORT" TAG="$STACK_TAG"
}

stack_dc() {
  docker compose --project-name "$STACK_PROJECT" --project-directory "$STACK_REPO/deploy" \
    --env-file "$STACK_DIR/env" -f "$STACK_REPO/deploy/compose.yaml" "$@"
}

# stack_root CMD...: run CMD as root in a throwaway container with STACK_DIR at /s.
stack_root() {
  docker run --rm --network none -v "$STACK_DIR:/s" -v "$STACK_REPO/deploy:/deploy:ro" \
    -e SUDO_UID="$(id -u)" -e SUDO_GID="$(id -g)" "$STACK_HELPER_IMAGE" "$@"
}

# stack_init: checks, a clean environment, data directories with the container owners, the
# env file, raw data mounted.
stack_init() {
  : "${STACK_DIR:?}" "${STACK_TAG:?}"
  stack_check_project
  [[ -d "$STACK_DIR" ]] || sdie "STACK_DIR $STACK_DIR does not exist"
  [[ -d "$RAW_DIR" ]] || sdie "RAW_DIR $RAW_DIR does not exist (MovieLens root; see tools/download_movielens.sh)"
  stack_scrub_env
  mkdir -p "$STACK_DIR/data"
  chmod 755 "$STACK_DIR" "$STACK_DIR/data"
  stack_root env RECS_DATA_DIR=/s/data bash /deploy/init-data.sh >/dev/null
  # data/raw: read-only bind of the MovieLens directory (the train service mounts it :ro).
  rmdir "$STACK_DIR/data/raw"
  ln -s "$RAW_DIR" "$STACK_DIR/data/raw"
  install -m 0600 /dev/null "$STACK_DIR/env"
  cat > "$STACK_DIR/env" <<EOF
TAG=$STACK_TAG
RECS_SITE_ORIGIN=$STACK_ORIGIN
RECS_SIGNUP_MODE=invite
RECS_LOG_LEVEL=info
TMDB_API_KEY=
RECS_DATA_DIR=$STACK_DIR/data
RECS_HTTP_PORT=$STACK_PORT
SMOKE_USERNAME=smoke-test
SMOKE_PASSWORD=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
SMOKE_MODEL=pop
GRAFANA_ADMIN_PASSWORD=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
EOF
  # Configs for the train job (not baked into the train image): readable by its UID.
  mkdir -p "$STACK_DIR/configs"
  cp "$STACK_REPO"/configs/*.json "$STACK_DIR/configs/"
  chmod 755 "$STACK_DIR/configs"
  chmod 644 "$STACK_DIR"/configs/*.json
}

# stack_train DATASET MODELS DEFAULT: train an artifact with the train image and set CURRENT.
# Extra configs can be dropped into STACK_DIR/configs first.
stack_train() {
  local dataset="$1" models="$2" default="$3"
  # Otherwise Docker would bind a missing path by creating it (root-owned) on the host.
  [[ -d "$RAW_DIR/$dataset" ]] || sdie "$RAW_DIR/$dataset does not exist (tools/download_movielens.sh $dataset)"
  slog "training $models on $dataset (train image $STACK_TAG)"
  stack_dc run --rm --no-deps -v "$STACK_DIR/configs:/srv/configs:ro" train \
    train --dataset "$dataset" --models "$models" --default-model "$default" \
    --config-dir /srv/configs --set-current --seed 42
}

stack_up() {
  stack_dc up -d caddy api model >/dev/null
  local i
  for i in $(seq 1 120); do
    curl -fs -o /dev/null --max-time 2 -H "Host: ${STACK_ORIGIN#*://}" "http://127.0.0.1:$STACK_PORT/healthz" && return 0
    sleep 1
  done
  stack_dc logs --no-color --tail 50 >&2 || true
  slog "stack did not become healthy"
  return 1
}

# stack_invite USES: print a new invite code (recs-api admin invite create, as in the runbook).
# Fails, showing the command's stderr, unless the output is a well-formed code.
stack_invite() {
  local out err code
  out="$(mktemp "$STACK_DIR/invite.XXXXXX")" err="$(mktemp "$STACK_DIR/invite-err.XXXXXX")"
  if ! stack_dc run --rm --no-deps -T api admin invite create --uses "$1" --expires 1d --note integration >"$out" 2>"$err"; then
    cat "$err" >&2
    rm -f "$out" "$err"
    sdie "recs-api admin invite create failed"
  fi
  code="$(tail -n 1 "$out")"
  rm -f "$out"
  if [[ ! "$code" =~ ^([A-Z2-7]{4}-){6}[A-Z2-7]{2}$ ]]; then
    cat "$err" >&2
    rm -f "$err"
    sdie "invite create printed no valid code"
  fi
  rm -f "$err"
  printf '%s\n' "$code"
}

# stack_signup USER CODE: create an account through Caddy. The password is read from stdin, and
# the password and invite code reach curl through a 0600 file, never through any argv.
stack_signup() {
  local user="$1" code="$2" body
  body="$(mktemp "$STACK_DIR/signup.XXXXXX")"
  { cat; printf '\n%s' "$code"; } | python3 -c '
import json, sys
password, code = sys.stdin.read().rsplit("\n", 1)
print(json.dumps({"username": sys.argv[1], "password": password, "invite_code": code}))' "$user" > "$body"
  local rc=0
  curl -fsS -o /dev/null --max-time 10 -X POST -H "Host: ${STACK_ORIGIN#*://}" -H "Origin: $STACK_ORIGIN" \
    -H 'X-Requested-With: recs' -H 'Content-Type: application/json' --data-binary "@$body" \
    "http://127.0.0.1:$STACK_PORT/api/auth/signup" || rc=$?
  rm -f "$body"
  return $rc
}

# stack_down: remove the project's containers, networks and volumes, then the scratch data
# (owned by the container UIDs, so removed as root in a throwaway container). Never stops
# early: every step runs even if an earlier one fails.
stack_down() {
  set +e
  [[ -n "${STACK_DIR:-}" && -d "$STACK_DIR" ]] || return 0
  if [[ -f "$STACK_DIR/env" && -n "${STACK_PROJECT:-}" ]]; then
    stack_dc --profile jobs down --volumes --remove-orphans >/dev/null 2>&1
  fi
  [[ -L "$STACK_DIR/data/raw" ]] && rm -f "$STACK_DIR/data/raw"
  [[ -d "$STACK_DIR/data" ]] && stack_root rm -rf /s/data >/dev/null 2>&1
  rm -rf "$STACK_DIR"
  return 0
}
