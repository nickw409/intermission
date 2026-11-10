#!/usr/bin/env bash
# recs-train train picks each model's config as <config-dir>/<model>.<dataset>.tuned.json if it
# exists, else <config-dir>/<model>.json, else the model defaults. Run by ctest with the binary.
set -euo pipefail
bin="${1:?usage: config_lookup.sh <recs-train> <tiny fixture dir>}"
fixture="${2:?usage: config_lookup.sh <recs-train> <tiny fixture dir>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/data/raw/tiny" "$work/cfg"
cp "$fixture"/*.csv "$work/data/raw/tiny/"

train() { # train <out dir>; each run gets its own artifact root (versions are named by the second)
  "$bin" train --dataset tiny --models pop --default-model pop --data-dir "$work/data" \
    --out "$1" --config-dir "$work/cfg" --seed 1 | grep '^fit pop '
}
n=0
check() { # check <label> <expected m>
  n=$((n + 1))
  local line; line="$(train "$work/art$n")"
  if [[ "$line" == *"\"m\":$2"* ]]; then echo "ok: $1 ($line)"; else echo "FAIL: $1: want m=$2, got: $line"; exit 1; fi
}

echo '{ "m": 7 }' > "$work/cfg/pop.json"
echo '{ "m": 3 }' > "$work/cfg/pop.tiny.tuned.json"
echo '{ "m": 99 }' > "$work/cfg/pop.other.tuned.json"
check "dataset-specific tuned config wins" 3
rm "$work/cfg/pop.tiny.tuned.json"
check "falls back to <model>.json" 7
