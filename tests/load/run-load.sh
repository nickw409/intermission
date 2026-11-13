#!/usr/bin/env bash
# Gate N3 load: /api/recommend (item-KNN, recommendation-cache miss on every request) through
# Caddy, p99 < 75 ms at 20 concurrent clients, on the real compose stack on 127.0.0.1.
#
#   tests/load/run-load.sh <tag> [dataset]        # dataset: ml-latest-small (default) | ml-32m
#
#   KNN_CONFIG=path   KNN config for the artifact (default: configs/knn.<dataset>.tuned.json if
#                     it exists, else configs/knn.json)
#   LOAD_USERS=70 LOAD_CLIENTS=20 LOAD_ARGS="..."   passed to the load generator
#   LOAD_GENRES=list  genre filter (e.g. Film-Noir or Horror,Thriller) added to every recommend
#                     request as &genres= (-genres); default none. Named in the label and the
#                     summary file name.
#   LOAD_OUT=path     JSON summary (default data/load-results/<UTC time>-<dataset>.json; data/ is
#                     gitignored)
#   KEEP_STACK=1      leave the stack running (prints the teardown command)
#   LOAD_ARTIFACTS=dir  serve an existing model instead of training one: dir holds CURRENT and the
#                     artifact it names (for production: /srv/intermission/data/artifacts). The
#                     version CURRENT names is copied into the scratch stack by a root helper
#                     container, read-only, so neither MovieLens data nor training is needed. Use
#                     it on the serving host (the Pi), which cannot train; the dataset argument
#                     then only labels the result. The artifact must contain knn.
#
# The load generator is built with the host's Go if there is one, else in the pinned Go image
# (network none; it uses the standard library only), so the serving host needs only Docker.
#
# The sign-off measurement is ml-32m with the tuned KNN config (configs/knn.ml-32m.tuned.json);
# ml-latest-small is a quick check. The Phase 2 ml-32m run must also include filtered passes
# (LOAD_GENRES=Film-Noir, narrow, and all 19 genres) next to the unfiltered one.
#
# Trains pop + knn (default knn) on the dataset with the train image into a scratch data dir,
# starts a scratch compose project (intermission-load, 127.0.0.1:${STACK_PORT:-18091}), runs
# tests/load (main.go) and tears everything down. Wait for other heavy jobs (fuzzers, builds)
# to finish first: the measurement is only meaningful on a quiet host. The script prints the
# load average and any running fuzzers before it starts.
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STACK_PORT="${STACK_PORT:-18091}" # before stack-lib.sh, which defaults it to 18090
# shellcheck source=../integration/stack-lib.sh
source "$here/../integration/stack-lib.sh"

STACK_TAG="${1:?usage: run-load.sh <tag> [dataset]}"
dataset="${2:-ml-latest-small}"
genres="${LOAD_GENRES:-}"
[[ "$genres" =~ ^([A-Za-z-]+(,[A-Za-z-]+){0,18})?$ ]] || { echo "LOAD_GENRES: comma-separated genre names, no spaces" >&2; exit 2; }
STACK_PROJECT=intermission-load # fixed: never inherited (stack_check_project refuses real stacks)
stack_check_project
STACK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/intermission-load.XXXXXX")"
if [[ -z "${KEEP_STACK:-}" ]]; then
  trap stack_down EXIT
else
  trap 'slog "kept; tear down with: STACK_PROJECT=$STACK_PROJECT STACK_DIR=$STACK_DIR bash -c \"source $here/../integration/stack-lib.sh; stack_down\""' EXIT
fi
suffix=""
if [[ -n "$genres" ]]; then
  n_genres="$(tr ',' '\n' <<<"$genres" | wc -l)"
  if (( n_genres == 1 )); then suffix="-$genres"; else suffix="-${n_genres}genres"; fi
fi
out="${LOAD_OUT:-$STACK_REPO/data/load-results/$(date -u +%Y%m%dT%H%M%SZ)-$dataset$suffix.json}"
mkdir -p "$(dirname "$out")"

artifacts="${LOAD_ARTIFACTS:-}"
if [[ -n "$artifacts" ]]; then
  [[ -d "$artifacts" ]] || { echo "LOAD_ARTIFACTS: $artifacts is not a directory" >&2; exit 2; }
  # Absolute and colon-free: docker -v would read a bare name as a (new, empty) named volume.
  artifacts="$(cd "$artifacts" && pwd -P)"
  [[ "$artifacts" != *:* ]] || { echo "LOAD_ARTIFACTS: a path with ':' cannot be mounted" >&2; exit 2; }
  # No MovieLens data is needed: stack_init only checks that the directory exists.
  RAW_DIR="$STACK_DIR/no-raw"
  mkdir -p "$RAW_DIR"
else
  knn_config="${KNN_CONFIG:-}"
  if [[ -z "$knn_config" ]]; then
    knn_config="$STACK_REPO/configs/knn.$dataset.tuned.json"
    [[ -f "$knn_config" ]] || knn_config="$STACK_REPO/configs/knn.json"
  fi
fi

stack_init
if [[ -n "$artifacts" ]]; then
  # Copy only the version CURRENT names, as root (production artifacts are 10003:10000, 2750),
  # from a read-only mount; the scratch copy gets the owners recs-train would have given it.
  version="$(docker run --rm --network none -v "$artifacts:/src:ro" -v "$STACK_DIR:/s" \
    "$STACK_HELPER_IMAGE" bash -c '
      set -euo pipefail
      v="$(cat /src/CURRENT)"
      # A leading letter or digit: rules out "." and "..", which would copy the wrong tree.
      [[ "$v" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo "CURRENT does not name an artifact: $v" >&2; exit 1; }
      [[ -d "/src/$v" && ! -L "/src/$v" ]] || { echo "artifact $v is missing" >&2; exit 1; }
      if [[ -n "$(find "/src/$v" -type l -print -quit)" ]]; then
        echo "artifact $v contains symlinks; refusing to copy it" >&2; exit 1
      fi
      cp -a "/src/$v" /s/data/artifacts/
      printf "%s\n" "$v" > /s/data/artifacts/CURRENT
      chown -R 10003:10000 /s/data/artifacts
      chmod -R g+rX,o-rwx /s/data/artifacts/"$v" /s/data/artifacts/CURRENT
      echo "$v"')"
  label="$dataset, artifact $version (LOAD_ARTIFACTS)"
  # The serving host runs production next to this stack: cap the scratch recs-model well below
  # compose.yaml's 4g default (it peaks around 160 MiB with all six ml-32m models).
  echo "MODEL_MEM_LIMIT=${LOAD_MODEL_MEM_LIMIT:-1g}" >> "$STACK_DIR/env"
  slog "serving artifact $version copied from $artifacts (no training)"
else
  # recs-train prefers knn.<dataset>.tuned.json over knn.json; remove the copied tuned files so
  # the config requested here is the one trained (and named in the result label).
  rm -f "$STACK_DIR"/configs/knn.*.tuned.json
  install -m 0644 "$knn_config" "$STACK_DIR/configs/knn.json"
  slog "knn config ($knn_config): $(cat "$knn_config")"
  stack_train "$dataset" pop,knn knn
  label="$dataset, knn $(tr -d '\n ' < "$knn_config")"
fi
stack_up
code="$(stack_invite "$(( ${LOAD_USERS:-70} + 5 ))")"
[[ -n "$code" ]] || { slog "no invite code"; exit 1; }

if command -v go >/dev/null; then
  (cd "$here" && go build -o "$STACK_DIR/recs-load" .)
else
  slog "no Go toolchain on this host: building the load generator in the pinned Go image"
  docker run --rm --network none --user "$(id -u):$(id -g)" \
    -e GOCACHE=/tmp/gocache -e GOPATH=/tmp/gopath -e GOTOOLCHAIN=local \
    -v "$here:/src:ro" -v "$STACK_DIR:/out" -w /src \
    golang:1.27.4-trixie@sha256:b54b2639de8c1bfe538ec5ffc45f29b1932235ac5f6d63c059853646677112d9 \
    go build -buildvcs=false -o /out/recs-load .
fi
# Between setup and measurement: report the host state; with WAIT_QUIET=1, first wait until no
# fuzzer is running (other load is reported, not waited for).
# (Bracketed so the pattern does not match the shell running it.)
fuzz_re='[b]uild/fuzz/|-test[.]fu[z]z|go test .*-fu[z]z'
quiet="$(cat <<EOF
if [ -n "${WAIT_QUIET:-}" ]; then
  until ! pgrep -f '$fuzz_re' >/dev/null; do sleep 10; done
fi
echo "host: $(nproc) CPUs, load average \$(cut -d' ' -f1-3 /proc/loadavg)" >&2
if pgrep -f '$fuzz_re' >/dev/null; then echo "WARNING: fuzzers are running; latency numbers will be skewed" >&2; fi
EOF
)"
# shellcheck disable=SC2086
LOAD_INVITE="$code" "$STACK_DIR/recs-load" -before-measure "$quiet" -base "http://127.0.0.1:$STACK_PORT" -origin "$STACK_ORIGIN" \
  -out "$out" -users "${LOAD_USERS:-70}" -clients "${LOAD_CLIENTS:-20}" -genres "$genres" \
  -label "$label${genres:+, genres $genres}" \
  ${LOAD_ARGS:-}
