#!/usr/bin/env bash
# Runs every libFuzzer harness over its committed corpus, then fuzzes it for a fixed time.
#
#   tools/fuzz/replay.sh [seconds per harness, default 60] [harness name filter (regex)]
#
# Needs the fuzz build first:  cmake --preset fuzz && cmake --build --preset fuzz -j3
#
# Environment:
#   FUZZ_BUILD  the fuzz build directory (default: build/fuzz)
#   FUZZ_OUT    where logs and crash inputs go (default: $FUZZ_BUILD/fuzz-out); must be inside
#               FUZZ_BUILD, because each harness's directory there is deleted before its run
#
# Per harness:
#   1. replay:  -runs=0 over the committed corpus (a regression check: every saved input must pass)
#   2. fuzz:    -max_total_time=<seconds>; new coverage goes to a scratch corpus, never into the
#               committed one, so the tree stays clean. Crash/leak/timeout inputs are written to
#               $FUZZ_OUT/<harness>/ for triage.
# seconds = 0 runs the replay only (deterministic; what the required CI check uses).
# FUZZ_SEED=<n> fixes libFuzzer's seed for the fuzz step (it is printed either way).
# On a failure the last lines of the log are printed, so CI output shows the cause.
# Every harness needs a committed corpus (tests/fuzz/<group>/corpus/<harness>/); a missing one is
# a failure. Exits non-zero if any harness fails either step. Summary lines:
# "<harness> replay|fuzz ok|FAIL".
set -euo pipefail
shopt -s nullglob

seconds="${1:-60}"
filter="${2:-.}"
per_input_timeout=25  # seconds; libFuzzer reports a slower input as a timeout
seed="${FUZZ_SEED:-$(( (RANDOM << 15 | RANDOM) + 1 ))}"
[[ "$seconds" =~ ^[0-9]+$ ]] || { echo "seconds must be a whole number" >&2; exit 1; }
[[ "$seed" =~ ^[0-9]+$ ]] && (( seed >= 1 && seed <= 4294967295 )) || { echo "FUZZ_SEED must be 1..4294967295 (libFuzzer seeds are 32-bit)" >&2; exit 1; }
if [[ "$seconds" -gt 0 ]]; then echo "fuzz seed: $seed (rerun with FUZZ_SEED=$seed)"; fi
fail_tail() { echo "---- last lines of $1"; tail -n 40 "$1" | sed 's/^/    /'; echo "----"; }
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${FUZZ_BUILD:-$root/build/fuzz}"
if [[ ! -d "$build" ]]; then
  echo "fuzz build directory $build not found (build the fuzz preset first)" >&2
  exit 1
fi
build="$(cd "$build" && pwd -P)"
out_root="${FUZZ_OUT:-$build/fuzz-out}"
mkdir -p "$out_root"
out_root="$(cd "$out_root" && pwd -P)"
case "$out_root/" in
  "$build"/?*) ;;
  *) echo "FUZZ_OUT ($out_root) must be a directory inside FUZZ_BUILD ($build)" >&2; exit 1 ;;
esac

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
  harnesses=("$src"/*.cpp)
  if [[ ${#harnesses[@]} -eq 0 ]]; then
    echo "no harnesses in $src" >&2
    failed=1
    continue
  fi
  for f in "${harnesses[@]}"; do
    name="$(basename "$f" .cpp)"
    [[ "$name" =~ $filter ]] || continue
    bin="$bin_dir/fuzz_$name"
    corpus="$src/corpus/$name"
    ran=$((ran + 1))
    if [[ ! -x "$bin" ]]; then
      echo "$name FAIL: missing $bin (build the fuzz preset first)"
      failed=1
      continue
    fi
    if [[ ! -d "$corpus" ]] || [[ -z "$(ls -A "$corpus")" ]]; then
      echo "$name FAIL: no committed corpus in $corpus"
      failed=1
      continue
    fi
    out="$out_root/$name"
    # Guarded delete: only ever a harness directory strictly inside the build directory.
    case "$out" in
      "$build"/?*/"$name") rm -rf -- "$out" ;;
      *) echo "refusing to delete $out" >&2; exit 1 ;;
    esac
    mkdir -p "$out/new"

    if "$bin" -runs=0 -timeout="$per_input_timeout" "$corpus" >"$out/replay.log" 2>&1; then
      echo "$name replay ok"
    else
      echo "$name replay FAIL (log: $out/replay.log)"
      fail_tail "$out/replay.log"
      failed=1
      continue
    fi

    [[ "$seconds" -eq 0 ]] && continue
    if "$bin" -seed="$seed" -max_total_time="$seconds" -timeout="$per_input_timeout" -rss_limit_mb=2048 -print_final_stats=1 \
         -artifact_prefix="$out/" "$out/new" "$corpus" >"$out/fuzz.log" 2>&1; then
      execs="$(grep -o 'stat::number_of_executed_units: *[0-9]*' "$out/fuzz.log" | grep -o '[0-9]*$' || echo '?')"
      echo "$name fuzz ok (${seconds}s, $execs execs)"
    else
      echo "$name fuzz FAIL (log: $out/fuzz.log; inputs in $out/)"
      fail_tail "$out/fuzz.log"
      failed=1
    fi
  done
done

if [[ "$ran" -eq 0 ]]; then
  echo "no harness matched '$filter'" >&2
  exit 1
fi
exit "$failed"
