#!/usr/bin/env bash
# Runs every libFuzzer harness over its committed corpus, then fuzzes it for a fixed time.
#
#   tools/fuzz/replay.sh [seconds per harness, default 60] [harness name filter (regex)]
#
# Needs the fuzz build first:  cmake --preset fuzz && cmake --build --preset fuzz -j3
#
# Per harness:
#   1. replay:  -runs=0 over the committed corpus (a regression check: every saved input must pass)
#   2. fuzz:    -max_total_time=<seconds>; new coverage goes to a scratch corpus, never into the
#               committed one, so the tree stays clean. Crash/leak/timeout inputs are written to
#               $FUZZ_OUT (default build/fuzz/fuzz-out/<harness>/) for triage.
# Exits non-zero if any harness fails either step. Summary lines: "<harness> replay|fuzz ok|FAIL".
set -euo pipefail

seconds="${1:-60}"
filter="${2:-.}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$root/build/fuzz"
out_root="${FUZZ_OUT:-$build/fuzz-out}"

# harness source dir : binary dir (see model-service/CMakeLists.txt and tests/fuzz/core/CMakeLists.txt)
groups=(
  "tests/fuzz/model:model-service"
  "tests/fuzz/core:fuzz-core"
)

failed=0
ran=0
for g in "${groups[@]}"; do
  src="$root/${g%%:*}"
  bin_dir="$build/${g##*:}"
  for f in "$src"/*.cpp; do
    name="$(basename "$f" .cpp)"
    [[ "$name" =~ $filter ]] || continue
    bin="$bin_dir/fuzz_$name"
    corpus="$src/corpus/$name"
    if [[ ! -x "$bin" ]]; then
      echo "$name: missing $bin (build the fuzz preset first)" >&2
      failed=1
      continue
    fi
    mkdir -p "$corpus"
    out="$out_root/$name"
    rm -rf "$out"
    mkdir -p "$out/new"
    ran=$((ran + 1))

    if "$bin" -runs=0 "$corpus" >"$out/replay.log" 2>&1; then
      echo "$name replay ok"
    else
      echo "$name replay FAIL (log: $out/replay.log)"
      failed=1
      continue
    fi

    if "$bin" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
         -artifact_prefix="$out/" "$out/new" "$corpus" >"$out/fuzz.log" 2>&1; then
      execs="$(grep -o 'stat::number_of_executed_units: *[0-9]*' "$out/fuzz.log" | grep -o '[0-9]*$' || echo '?')"
      echo "$name fuzz ok (${seconds}s, $execs execs)"
    else
      echo "$name fuzz FAIL (log: $out/fuzz.log; inputs in $out/)"
      failed=1
    fi
  done
done

if [[ "$ran" -eq 0 ]]; then
  echo "no harness matched '$filter'" >&2
  exit 1
fi
exit "$failed"
