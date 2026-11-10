#!/usr/bin/env bash
# recs-train train picks each model's config as <config-dir>/<model>.<dataset>.tuned.json if it
# exists, else <config-dir>/<model>.json, else the model defaults, and logs which one it used.
# Run by ctest with the binary and the tiny fixture.
set -euo pipefail
bin="${1:?usage: config_lookup.sh <recs-train> <tiny fixture dir>}"
fixture="${2:?usage: config_lookup.sh <recs-train> <tiny fixture dir>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/data/raw/tiny" "$work/cfg"
cp "$fixture"/*.csv "$work/data/raw/tiny/"

n=0
check() { # check <label> <expected m, as printed> <expected config line>
  n=$((n + 1))
  # Each run gets its own artifact root: versions are named by the second.
  local out
  out="$("$bin" train --dataset tiny --models pop --default-model pop --data-dir "$work/data" \
    --out "$work/art$n" --config-dir "$work/cfg" --seed 1)"
  local fit; fit="$(grep '^fit pop ' <<<"$out")"
  if [[ "$fit" != *"config {\"m\":$2}" ]]; then echo "FAIL: $1: want m=$2, got: $fit"; exit 1; fi
  if ! grep -qxF "$3" <<<"$out"; then echo "FAIL: $1: log lacks '$3'"; echo "$out"; exit 1; fi
  echo "ok: $1"
}

echo '{ "m": 7 }' > "$work/cfg/pop.json"
echo '{ "m": 3 }' > "$work/cfg/pop.tiny.tuned.json"
echo '{ "m": 99 }' > "$work/cfg/pop.other.tuned.json"   # another dataset's: never used here
check "dataset-specific tuned config wins" 3.0 "config pop: $work/cfg/pop.tiny.tuned.json"
rm "$work/cfg/pop.tiny.tuned.json"
check "falls back to <model>.json" 7.0 "config pop: $work/cfg/pop.json"
rm "$work/cfg/pop.json"
check "model defaults when neither exists" 10.0 \
  "config pop: model defaults (no pop.tiny.tuned.json or pop.json in $work/cfg)"

# Dataset names become file names; anything outside [A-Za-z0-9._-] is refused before use.
if "$bin" train --dataset '../tiny' --models pop --data-dir "$work/data" --out "$work/artx" \
     --config-dir "$work/cfg" >"$work/bad.log" 2>&1; then
  echo "FAIL: a dataset name with '/' was accepted"; exit 1
fi
grep -q "invalid dataset name" "$work/bad.log" && echo "ok: invalid dataset name refused" \
  || { echo "FAIL: unexpected error for a bad dataset name:"; cat "$work/bad.log"; exit 1; }
