#!/usr/bin/env bash
# Tests for release.sh with no SHA (lib.sh latest_green_main / commit_green / moves_forward and
# obtain_images --verified-only), with curl, docker, fetch-images.sh and build.sh stubbed, in a
# throwaway repository whose origin/main has commits c1 (oldest) .. c4:
#   - the newest commit whose latest push runs of ci.yml and release.yml both succeeded is chosen
#   - running, failed, cancelled and missing gate runs skip a commit; runs of other workflows (the
#     scheduled security scan), other events or other branches neither block nor qualify one
#   - an API failure or an unreadable answer stops the search instead of falling back
#   - no qualifying commit in the window is an error
#   - moves_forward: same or newer is fine, older or diverged is refused, an unknown running TAG
#     fails closed, no TAG at all is fine
#   - release.sh with no SHA: never builds locally (fetch exit 2 stops it), refuses to go back,
#     refuses when the running TAG is unknown, works with --no-rollback; with a SHA the local-build
#     fallback still applies
#
#   deploy/tests/test_release_latest.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else.
# shellcheck disable=SC2015,SC2016,SC2181  # SC2016: stubs expand later; SC2181: exit codes 0/1/2 are compared
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }
g() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main "$@"; }

# origin (bare) with c1..c4 on main, plus a side commit (another line of history).
g init -q --bare "$tmp/origin.git"
g init -q "$tmp/seed"
for i in 1 2 3 4; do g -C "$tmp/seed" commit -q --allow-empty -m "c$i"; done
g -C "$tmp/seed" push -q "$tmp/origin.git" HEAD:main
g -C "$tmp/seed" checkout -q --orphan side && g -C "$tmp/seed" commit -q --allow-empty -m side
g -C "$tmp/seed" push -q "$tmp/origin.git" side:side
g clone -q "$tmp/origin.git" "$tmp/repo"
mkdir -p "$tmp/repo/deploy"
cp "$here/lib.sh" "$here/release.sh" "$tmp/repo/deploy/"
c() { git -C "$tmp/repo" rev-parse "origin/main~$((4 - $1))"; }   # c N: full SHA of commit cN
side="$(git -C "$tmp/repo" rev-parse origin/side)"

# Stubs. curl answers .../actions/runs?head_sha=<sha>... from $STUB_DIR/<sha>.json; a missing file,
# or STUB_API_FAIL=<sha>, is an HTTP error. fetch-images.sh / build.sh log themselves and exit
# with STUB_FETCH_RC / 0. docker reports no images present (images_present false).
mkdir -p "$tmp/bin" "$tmp/stub"
cat > "$tmp/bin/curl" <<'EOF'
#!/usr/bin/env bash
url="${*: -1}"; sha="${url#*head_sha=}"; sha="${sha%%&*}"
echo "$sha" >> "$STUB_LOG"
[[ "${STUB_API_FAIL:-}" == "$sha" || ! -f "$STUB_DIR/$sha.json" ]] && { echo "curl: (22) 403" >&2; exit 22; }
cat "$STUB_DIR/$sha.json"
EOF
printf '#!/usr/bin/env bash\nexit 1\n' > "$tmp/bin/docker"
printf '#!/usr/bin/env bash\necho "fetch $1" >> "$STUB_LOG"\nexit "${STUB_FETCH_RC:-0}"\n' > "$tmp/repo/deploy/fetch-images.sh"
printf '#!/usr/bin/env bash\necho "build $1" >> "$STUB_LOG"\nexit 0\n' > "$tmp/repo/deploy/build.sh"
chmod +x "$tmp/bin/curl" "$tmp/bin/docker" "$tmp/repo/deploy/fetch-images.sh" "$tmp/repo/deploy/build.sh"

# runs N RUN...: the Actions API answer for commit cN; wf PATH STATUS CONCLUSION [ID EVENT BRANCH]
runs() {
  local n="$1" sha; shift
  sha="$(c "$n")"
  local IFS=,
  printf '{"total_count": %d, "workflow_runs": [%s]}' "$#" "$(printf '%s' "$*" | sed "s/@SHA@/$sha/g")" \
    > "$tmp/stub/$sha.json"
}
wf() {
  printf '{"id": %s, "path": ".github/workflows/%s", "head_sha": "@SHA@", "event": "%s", "head_branch": "%s", "status": "%s", "conclusion": %s}' \
    "${4:-1}" "$1" "${5:-push}" "${6:-main}" "$2" "$3"
}
green=("$(wf ci.yml completed '"success"')" "$(wf release.yml completed '"success"')")

# pick [VAR=value...]: run latest_green_main; sets $rc, $out (stdout), $err (stderr).
pick() {
  : > "$tmp/log"
  out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" STUB_DIR="$tmp/stub" "$@" \
    bash -c "source '$tmp/repo/deploy/lib.sh'; latest_green_main" 2>"$tmp/err")"
  rc=$?
  err="$(cat "$tmp/err")"
}

echo "== newest commit green: chosen, one API call"
for n in 1 2 3 4; do runs "$n" "${green[@]}"; done
pick
[[ $rc -eq 0 && "$out" == "$(c 4)" ]] && ok "c4 chosen" || bad "rc=$rc out=$out err=$err"
[[ "$(wc -l < "$tmp/log")" -eq 1 ]] && ok "one API call" || bad "calls: $(wc -l < "$tmp/log")"

echo "== running, failed, missing gate runs: skipped"
runs 4 "$(wf ci.yml in_progress null)" "$(wf release.yml completed '"success"')"
runs 3 "$(wf ci.yml completed '"success"')" "$(wf release.yml completed '"failure"')"
runs 2 "$(wf ci.yml completed '"success"')"
runs 1 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 1)" ]] && ok "c1 chosen" || bad "rc=$rc out=$out err=$err"
grep -q "ci.yml still running" <<<"$err" && ok "running reason logged" || bad "err: $err"
grep -q "release.yml failure" <<<"$err" && ok "failure reason logged" || bad "err: $err"
grep -q "release.yml has not run" <<<"$err" && ok "missing reason logged" || bad "err: $err"

echo "== cancelled gate run: skipped"
runs 4 "$(wf ci.yml completed '"cancelled"')" "$(wf release.yml completed '"success"')"
runs 3 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 3)" ]] && ok "c3 chosen" || bad "rc=$rc out=$out err=$err"

echo "== a failed security scan on the head does not block it"
runs 4 "${green[@]}" "$(wf security-scan.yml completed '"failure"' 9 schedule)"
pick
[[ $rc -eq 0 && "$out" == "$(c 4)" ]] && ok "c4 chosen despite the scan" || bad "rc=$rc out=$out err=$err"

echo "== gate runs from another event or branch do not qualify"
runs 4 "$(wf ci.yml completed '"success"' 1 workflow_dispatch)" "$(wf release.yml completed '"success"' 2 push other)"
runs 3 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 3)" ]] && ok "c3 chosen" || bad "rc=$rc out=$out err=$err"

echo "== the newest run of a workflow decides"
runs 4 "$(wf ci.yml completed '"failure"' 1)" "$(wf ci.yml completed '"success"' 5)" "$(wf release.yml completed '"success"')"
pick
[[ $rc -eq 0 && "$out" == "$(c 4)" ]] && ok "newer success wins" || bad "rc=$rc out=$out err=$err"
runs 4 "$(wf ci.yml completed '"success"' 1)" "$(wf ci.yml completed '"failure"' 5)" "$(wf release.yml completed '"success"')"
runs 3 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 3)" ]] && ok "newer failure wins" || bad "rc=$rc out=$out err=$err"

echo "== API failure: stops, never falls back to an older commit"
for n in 1 2 3 4; do runs "$n" "${green[@]}"; done
pick STUB_API_FAIL="$(c 4)"
[[ $rc -ne 0 && -z "$out" ]] && ok "exit non-zero, nothing chosen" || bad "rc=$rc out=$out"
[[ "$(wc -l < "$tmp/log")" -eq 1 ]] && ok "stopped after the failed call" || bad "calls: $(wc -l < "$tmp/log")"
grep -q "cannot check CI status" <<<"$err" && ok "reason logged" || bad "err: $err"

echo "== unreadable API answer: stops"
echo 'not json' > "$tmp/stub/$(c 4).json"
pick
[[ $rc -ne 0 && -z "$out" ]] && ok "exit non-zero, nothing chosen" || bad "rc=$rc out=$out"

echo "== more runs than one page: stops, never skips back"
printf '{"total_count": 150, "workflow_runs": [%s]}' "$(IFS=,; printf '%s' "${green[*]}" | sed "s/@SHA@/$(c 4)/g")" > "$tmp/stub/$(c 4).json"
runs 3 "${green[@]}"
pick
[[ $rc -ne 0 && -z "$out" ]] && ok "exit non-zero, nothing chosen" || bad "rc=$rc out=$out"
[[ "$(wc -l < "$tmp/log")" -eq 1 ]] && ok "stopped at the paged commit" || bad "calls: $(wc -l < "$tmp/log")"

echo "== control characters in API text never reach the log"
runs 4 "$(wf ci.yml completed '"fail\\u001b[2Jure\\nFAKE LOG LINE"')" "$(wf release.yml completed '"success"')"
runs 3 "${green[@]}"
pick
grep -q $'\e' <<<"$err" && bad "escape character in log" || ok "no escape characters"
grep -q '^FAKE LOG LINE' <<<"$err" && bad "forged log line" || ok "no forged log line"

echo "== nothing qualifies: error"
for n in 1 2 3 4; do runs "$n" "$(wf ci.yml completed '"failure"')" "$(wf release.yml completed '"success"')"; done
pick
[[ $rc -ne 0 && -z "$out" ]] && ok "exit non-zero" || bad "rc=$rc out=$out"
grep -q "none of the last 20 commits" <<<"$err" && ok "reason logged" || bad "err: $err"

echo "== moves_forward"
mf() { bash -c "source '$tmp/repo/deploy/lib.sh'; rc=0; moves_forward '$1' '$2' || rc=\$?; exit \$rc"; }
mf "$(c 4)" "$(c 2 | cut -c1-7)"; [[ $? -eq 0 ]] && ok "newer: forward" || bad "c4 over c2 not forward"
mf "$(c 4)" "$(c 4 | cut -c1-7)"; [[ $? -eq 0 ]] && ok "same: forward" || bad "c4 over c4 not forward"
mf "$(c 4)" ""; [[ $? -eq 0 ]] && ok "nothing running: forward" || bad "empty TAG refused"
mf "$(c 2)" "$(c 4 | cut -c1-7)"; [[ $? -eq 1 ]] && ok "older: refused (1)" || bad "c2 over c4 not refused"
mf "$(c 4)" "${side:0:7}"; [[ $? -eq 1 ]] && ok "other line of history: refused (1)" || bad "diverged not refused"
mf "$(c 4)" "0000000"; [[ $? -eq 2 ]] && ok "unknown running TAG: fails closed (2)" || bad "unknown TAG not refused"

echo "== release.sh with no SHA"
mkdir -p "$tmp/data"/{db,exports,posters,reports,artifacts}
# rel TAG [ARGS...] [-- VAR=value...]: run release.sh with TAG in its env file.
rel() {
  local tagv="$1"; shift
  local args=() envs=()
  while [[ $# -gt 0 && "$1" != -- ]]; do args+=("$1"); shift; done
  [[ "${1:-}" == -- ]] && { shift; envs=("$@"); }
  printf 'TAG=%s\nRECS_SITE_ORIGIN=http://x\nSMOKE_PASSWORD=r\nGRAFANA_ADMIN_PASSWORD=r\n' "$tagv" > "$tmp/env"
  chmod 600 "$tmp/env"
  : > "$tmp/log"
  out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" STUB_DIR="$tmp/stub" RECS_ENV_FILE="$tmp/env" \
    RECS_LOCK_FILE="$tmp/ops.lock" RECS_DATA_DIR="$tmp/data" RECS_HISTORY_FILE="$tmp/history" \
    "${envs[@]}" "$tmp/repo/deploy/release.sh" "${args[@]}" 2>&1)"
  rc=$?
}
called() { grep -c "^$1 " "$tmp/log" || true; }
for n in 1 2 3 4; do runs "$n" "${green[@]}"; done

rel "$(c 2 | cut -c1-7)" -- STUB_FETCH_RC=2
[[ $rc -ne 0 ]] && ok "not published: exit non-zero" || bad "exit 0"
[[ "$(called fetch)" -eq 1 && "$(called build)" -eq 0 ]] && ok "fetched, never built locally" || bad "log: $(tr '\n' '|' < "$tmp/log")"
grep -q "only deploys verified registry images" <<<"$out" && ok "reason logged" || bad "out: $out"

rel "$(c 2 | cut -c1-7)" --no-rollback -- STUB_FETCH_RC=2
[[ $rc -ne 0 && "$(called build)" -eq 0 ]] && ok "--no-rollback without SHA: same rules" || bad "rc=$rc log: $(tr '\n' '|' < "$tmp/log")"

runs 4 "$(wf ci.yml completed '"failure"')" "$(wf release.yml completed '"success"')"
rel "$(c 4 | cut -c1-7)"
[[ $rc -ne 0 && "$(called fetch)" -eq 0 ]] && ok "running is newer than latest green: refused before fetching" || bad "rc=$rc log: $(tr '\n' '|' < "$tmp/log")"
grep -q "is not newer than the running version" <<<"$out" && ok "reason logged" || bad "out: $out"
runs 4 "${green[@]}"

rel "0000000"
[[ $rc -ne 0 && "$(called fetch)" -eq 0 ]] && ok "unknown running TAG: refused before fetching" || bad "rc=$rc log: $(tr '\n' '|' < "$tmp/log")"

echo "== release.sh with a SHA keeps the local-build fallback"
rel "$(c 2 | cut -c1-7)" "$(c 3)" -- STUB_FETCH_RC=2
[[ "$(called fetch)" -eq 1 && "$(called build)" -eq 1 ]] && ok "fetched, then built locally" || bad "log: $(tr '\n' '|' < "$tmp/log")"

if [[ $fails -gt 0 ]]; then
  echo "$fails check(s) failed" >&2
  exit 1
fi
echo "PASS: release.sh latest green"
