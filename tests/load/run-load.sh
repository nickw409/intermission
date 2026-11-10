#!/usr/bin/env bash
# Gate N3 load: /api/recommend (item-KNN, recommendation-cache miss on every request) through
# Caddy, p99 < 75 ms at 20 concurrent clients, on the real compose stack on 127.0.0.1.
#
#   tests/load/run-load.sh <tag> [dataset]        # dataset: ml-latest-small (default) | ml-32m
#
#   KNN_CONFIG=path   KNN config for the artifact (default: configs/knn.<dataset>.tuned.json if
#                     it exists, else configs/knn.json)
#   LOAD_USERS=70 LOAD_CLIENTS=20 LOAD_ARGS="..."   passed to the load generator
#   KEEP_STACK=1      leave the stack running
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
STACK_PROJECT="${STACK_PROJECT:-intermission-load}"
STACK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/intermission-load.XXXXXX")"
if [[ -z "${KEEP_STACK:-}" ]]; then trap stack_down EXIT; fi

knn_config="${KNN_CONFIG:-}"
if [[ -z "$knn_config" ]]; then
  knn_config="$STACK_REPO/configs/knn.$dataset.tuned.json"
  [[ -f "$knn_config" ]] || knn_config="$STACK_REPO/configs/knn.json"
fi

stack_init
install -m 0644 "$knn_config" "$STACK_DIR/configs/knn.json"
slog "knn config ($knn_config): $(cat "$knn_config")"
stack_train "$dataset" pop,knn knn
stack_up
code="$(stack_invite "$(( ${LOAD_USERS:-70} + 5 ))")"
[[ -n "$code" ]] || { slog "no invite code"; exit 1; }

(cd "$here" && go build -o "$STACK_DIR/recs-load" .)
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
"$STACK_DIR/recs-load" -before-measure "$quiet" -base "http://127.0.0.1:$STACK_PORT" -origin "$STACK_ORIGIN" -invite "$code" \
  -users "${LOAD_USERS:-70}" -clients "${LOAD_CLIENTS:-20}" -label "$dataset, knn $(tr -d '\n ' < "$knn_config")" \
  ${LOAD_ARGS:-}
