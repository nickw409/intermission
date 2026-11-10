# Helpers for running a scratch Intermission compose stack on 127.0.0.1 (integration and load
# gates). Source it; don't run it. Uses the production compose file and images exactly as the
# deploy scripts do, with:
#   - its own compose project name and loopback port, so it never touches another stack;
#   - its own data directory (STACK_DIR/data) and env file (STACK_DIR/env);
#   - MovieLens mounted read-only from RAW_DIR (never written).
#
# The data directories need the owners the containers run as (deploy/init-data.sh, which wants
# root). Without sudo, the helpers run init-data.sh, and the final cleanup, in a throwaway root
# container (Docker access is root-equivalent anyway).
#
#   STACK_PROJECT   compose project name            (required)
#   STACK_DIR       scratch directory               (required; created)
#   STACK_TAG       image tag (short SHA)            (required)
#   STACK_PORT      loopback port for caddy          (default 18090)
#   RAW_DIR         MovieLens root (holds ml-latest-small/, ml-32m/)  (default: repo data/raw)
# shellcheck shell=bash

STACK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STACK_PORT="${STACK_PORT:-18090}"
RAW_DIR="${RAW_DIR:-$STACK_REPO/data/raw}"
STACK_ORIGIN="${STACK_ORIGIN:-http://intermission.test}"
# A root-capable image with a shell, already pulled by deploy/build.sh (the C++ builder base).
STACK_HELPER_IMAGE="${STACK_HELPER_IMAGE:-ubuntu:noble-20261024.1@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852}"

slog() { printf '%s [stack] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }

stack_dc() {
  docker compose --project-name "$STACK_PROJECT" --project-directory "$STACK_REPO/deploy" \
    --env-file "$STACK_DIR/env" -f "$STACK_REPO/deploy/compose.yaml" "$@"
}

# stack_root CMD...: run CMD as root in a throwaway container with STACK_DIR at /s.
stack_root() {
  docker run --rm --network none -v "$STACK_DIR:/s" -v "$STACK_REPO/deploy:/deploy:ro" \
    -e SUDO_UID="$(id -u)" -e SUDO_GID="$(id -g)" "$STACK_HELPER_IMAGE" "$@"
}

# stack_init: data directories with the container owners, the env file, raw data mounted.
stack_init() {
  : "${STACK_PROJECT:?}" "${STACK_DIR:?}" "${STACK_TAG:?}"
  mkdir -p "$STACK_DIR/data"
  chmod 755 "$STACK_DIR" "$STACK_DIR/data"
  stack_root env RECS_DATA_DIR=/s/data bash /deploy/init-data.sh >/dev/null
  # data/raw: read-only bind of the MovieLens directory (the train service mounts it :ro).
  rmdir "$STACK_DIR/data/raw"
  ln -s "$RAW_DIR" "$STACK_DIR/data/raw"
  STACK_SMOKE_PASSWORD="$(head -c 24 /dev/urandom | base64 | tr -d '/+=')"
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
SMOKE_PASSWORD=$STACK_SMOKE_PASSWORD
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
stack_invite() {
  stack_dc run --rm --no-deps -T api admin invite create --uses "$1" --expires 1d --note integration 2>/dev/null | tail -n 1
}

# stack_signup USER PASSWORD CODE: create an account through Caddy.
stack_signup() {
  local body
  body="$(python3 -c 'import json,sys; print(json.dumps({"username": sys.argv[1], "password": sys.argv[2], "invite_code": sys.argv[3]}))' "$1" "$2" "$3")"
  curl -fsS -o /dev/null --max-time 10 -X POST -H "Host: ${STACK_ORIGIN#*://}" -H "Origin: $STACK_ORIGIN" \
    -H 'X-Requested-With: recs' -H 'Content-Type: application/json' --data-binary "$body" \
    "http://127.0.0.1:$STACK_PORT/api/auth/signup"
}

# stack_down: remove the project's containers, networks and volumes, then the scratch data
# (owned by the container UIDs, so removed as root in a throwaway container).
stack_down() {
  [[ -n "${STACK_DIR:-}" && -d "$STACK_DIR" ]] || return 0
  [[ -f "$STACK_DIR/env" ]] && stack_dc --profile jobs down --volumes --remove-orphans >/dev/null 2>&1 || true
  [[ -L "$STACK_DIR/data/raw" ]] && rm -f "$STACK_DIR/data/raw"
  stack_root rm -rf /s/data >/dev/null 2>&1 || true
  rm -rf "$STACK_DIR"
}
