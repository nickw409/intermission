#!/usr/bin/env bash
# Unit tests for deploy/lib.sh guards:
#   - placeholder secrets (change-me*) are refused before anything starts
#   - deploy/.env is forced to mode 0600
#   - short_sha only accepts commits that exist in this repository
#   - one shared lock serialises release/backup/nightly/restore-drill; nested calls pass
#   - training_changed: which commit pairs recommend a retrain
#   - check_artifact: exit codes, the old-image skip and fail-closed start errors (compose stubbed)
#
#   deploy/tests/test_lib.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else; snippets passed to
# run_lib are single-quoted on purpose (they expand inside the child shell).
# shellcheck disable=SC2015,SC2016
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }

# run_lib SNIPPET: run SNIPPET in a fresh bash with lib.sh sourced; prints combined output.
run_lib() {
  RECS_ENV_FILE="$tmp/env" RECS_LOCK_FILE="$tmp/ops.lock" \
    bash -c "source '$here/lib.sh'; $1" 2>&1
}

good_env() {
  cat > "$tmp/env" <<'EOF'
TAG=0000000
RECS_SITE_ORIGIN=http://localhost:8080
SMOKE_USERNAME=smoke-test
SMOKE_PASSWORD=a-real-long-random-test-password
GRAFANA_ADMIN_PASSWORD=another-real-password
EOF
  chmod 600 "$tmp/env"
}

echo "== placeholder secrets"
good_env
if out="$(run_lib 'require_env_file; echo PASSED')" && grep -q PASSED <<<"$out"; then
  ok "real values accepted"
else
  bad "real values rejected: $out"
fi
for key in SMOKE_PASSWORD GRAFANA_ADMIN_PASSWORD TMDB_API_KEY; do
  good_env
  echo "$key=change-me-please" >> "$tmp/env"
  if out="$(run_lib 'require_env_file; echo PASSED')"; then
    bad "$key=change-me-please accepted"
  elif grep -q "$key" <<<"$out" && ! grep -q 'change-me-please' <<<"$out"; then
    ok "$key placeholder refused (value not echoed)"
  else
    bad "$key refusal message: $out"
  fi
done
good_env
cp "$here/.env.example" "$tmp/env"; chmod 600 "$tmp/env"
if run_lib 'require_env_file' >/dev/null; then bad ".env.example accepted as-is"; else ok ".env.example refused as-is"; fi

echo "== env file mode"
good_env
chmod 644 "$tmp/env"
run_lib 'require_env_file' >/dev/null
mode="$(stat -c %a "$tmp/env")"
[[ "$mode" == 600 ]] && ok "0644 env file tightened to 0600" || bad "env file mode is $mode after require_env_file"

echo "== short_sha"
good_env
head="$(git -C "$here" rev-parse --short=7 HEAD)"
out="$(run_lib 'short_sha HEAD')"
[[ "$out" == "$head" ]] && ok "HEAD -> $head" || bad "short_sha HEAD gave '$out'"
if out="$(run_lib 'short_sha deadbee')"; then bad "unknown SHA accepted: $out"; else ok "unknown SHA refused"; fi
if out="$(run_lib 'short_sha "x; rm -rf /"')"; then bad "garbage ref accepted: $out"; else ok "garbage ref refused"; fi

echo "== shared lock"
good_env
# Hold the lock in the background for 5 s.
RECS_ENV_FILE="$tmp/env" RECS_LOCK_FILE="$tmp/ops.lock" \
  bash -c "source '$here/lib.sh'; acquire_lock holder 5; touch '$tmp/held'; sleep 5" &
holder=$!
for _ in $(seq 1 50); do [[ -f "$tmp/held" ]] && break; sleep 0.1; done
if out="$(run_lib 'acquire_lock second 1; echo GOT')"; then
  bad "second holder got the lock: $out"
else
  grep -q 'holder' <<<"$out" && ok "second operation refused, names the holder" || ok "second operation refused"
fi
wait "$holder"
out="$(run_lib 'acquire_lock outer 1; bash -c "source \"$DEPLOY_DIR/lib.sh\"; acquire_lock inner 1; echo NESTED"')"
grep -q NESTED <<<"$out" && ok "nested script reuses the held lock" || bad "nested acquire failed: $out"
out="$(run_lib 'acquire_lock again 1; echo GOT')"
grep -q GOT <<<"$out" && ok "lock free again after the holder exits" || bad "lock not released: $out"

echo "== training_changed"
# A throwaway repository: base commit, then one commit outside the training source, then one
# inside it. REPO_DIR is reassigned after lib.sh is sourced.
repo="$tmp/repo"
git init -q "$repo"
g() { git -C "$repo" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false "$@"; }
mkdir -p "$repo/core" "$repo/web" "$repo/configs"
echo a > "$repo/core/model.cpp"; echo a > "$repo/web/app.tsx"; echo a > "$repo/configs/knn.json"
g add -A && g commit -q -m base
base="$(g rev-parse --short=7 HEAD)"
echo b > "$repo/web/app.tsx"; g commit -q -am web
web="$(g rev-parse --short=7 HEAD)"
echo b > "$repo/configs/knn.json"; g commit -q -am configs
cfg="$(g rev-parse --short=7 HEAD)"
tc() { run_lib "REPO_DIR='$repo'; training_changed $1 $2 && echo RC0 || echo RC\$?"; }
[[ "$(tc "$base" "$web")" == RC1 ]] && ok "web-only change: no retrain" || bad "web-only change gave $(tc "$base" "$web")"
[[ "$(tc "$base" "$cfg")" == RC0 ]] && ok "configs change: retrain recommended" || bad "configs change gave $(tc "$base" "$cfg")"
[[ "$(tc "$web" "$web")" == RC1 ]] && ok "same commit: no retrain" || bad "same commit gave $(tc "$web" "$web")"
[[ "$(tc unknown "$cfg")" == RC2 ]] && ok "git_sha unknown: cannot tell" || bad "unknown gave $(tc unknown "$cfg")"
[[ "$(tc deadbee "$cfg")" == RC2 ]] && ok "commit not in repo: cannot tell" || bad "missing commit gave $(tc deadbee "$cfg")"
[[ "$(tc "'-p'" "$cfg")" == RC2 ]] && ok "option-like git_sha refused" || bad "option-like gave $(tc "'-p'" "$cfg")"

# A branch named like a commit id prefix must not be resolved instead of the commit.
g branch "$base" "$cfg"
[[ "$(tc "$base" "$cfg")" == RC0 ]] && ok "hex-named branch ignored, the commit is used" || bad "hex-named branch gave $(tc "$base" "$cfg")"
for f in cmake/deps.cmake CMakeLists.txt CMakePresets.json deploy/docker/train.Dockerfile; do
  mkdir -p "$repo/$(dirname "$f")"; echo a > "$repo/$f"; g add -A; g commit -q -m "add $f"
  before="$(g rev-parse --short=7 HEAD)"
  echo b > "$repo/$f"; g commit -q -am "change $f"
  [[ "$(tc "$before" "$(g rev-parse --short=7 HEAD)")" == RC0 ]] && ok "$f change: retrain recommended" || bad "$f change not detected"
done

echo "== check_artifact"
# compose is stubbed: --help prints $HELP (exit $HELP_RC); the check prints $OUT (exit $CHECK_RC).
# The stub also records the arguments, so the test sees which service and TAG were used.
ca() {
  run_lib "compose() { echo \"TAG=\$TAG \$*\" >> '$tmp/compose.args'
    if [[ \" \$* \" == *' --help '* ]]; then echo \"\$HELP\"; return \"\$HELP_RC\"; fi
    [[ -n \"\$OUT\" ]] && echo \"\$OUT\"; return \"\$CHECK_RC\"; }
  out=\"\$(check_artifact abc1234)\" && rc=0 || rc=\$?; echo \"rc=\$rc out=[\$out]\""
}
: > "$tmp/compose.args"
out="$(HELP='--artifacts --check' HELP_RC=0 OUT=$'version=v1\ngit_sha=abc1234' CHECK_RC=0 ca)"
grep -q 'rc=0 out=\[version=v1' <<<"$out" && ok "check passes: output passed through" || bad "pass case: $out"
grep -q '^TAG=abc1234 run --rm --no-deps --pull never -T model-check$' "$tmp/compose.args" \
  && ok "runs model-check with the release's TAG" || bad "compose called as: $(cat "$tmp/compose.args")"
grep -qw 'model' <(sed 's/model-check//g' "$tmp/compose.args") && bad "used the live model service" || ok "never uses the live model service"
out="$(HELP='--artifacts --check' HELP_RC=0 OUT='' CHECK_RC=3 ca)"
grep -q 'rc=3 out=\[\]' <<<"$out" && ok "refused artifact: exit 3" || bad "refused case: $out"
out="$(HELP='--artifacts --check' HELP_RC=0 OUT='' CHECK_RC=4 ca)"
grep -q 'rc=4 out=\[\]' <<<"$out" && ok "no CURRENT: exit 4" || bad "no-CURRENT case: $out"
out="$(HELP='--artifacts --socket' HELP_RC=0 OUT='' CHECK_RC=0 ca)"
grep -q 'rc=0 out=\[\]' <<<"$out" && grep -q 'predates --check' <<<"$out" \
  && ok "image without --check: skipped with a warning" || bad "old-image case: $out"
out="$(HELP='Error response from daemon: no such image' HELP_RC=1 OUT='' CHECK_RC=0 ca)"
grep -q 'rc=2 out=\[\]' <<<"$out" && grep -q 'no such image' <<<"$out" \
  && ok "image that cannot start: fails, with the reason" || bad "start-failure case: $out"

if [[ $fails -gt 0 ]]; then
  echo "FAIL: $fails lib check(s) failed" >&2
  exit 1
fi
echo "PASS: lib.sh guards"
