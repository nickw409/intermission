#!/usr/bin/env bash
# Tests for lib.sh latest_green_main / commit_green / is_rollback (release.sh with no SHA), with
# curl stubbed and a throwaway repository whose origin/main has commits c1 (oldest) .. c4:
#   - the newest commit whose checks all passed and whose image set was signed is chosen
#   - pending, failed, unpublished (sign check skipped or missing) and empty commits are skipped
#   - an API failure stops the search instead of falling back to an older commit
#   - no qualifying commit in the window is an error
#   - is_rollback: an ancestor of the running TAG is a rollback; the same or a newer commit is not
#
#   deploy/tests/test_release_latest.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else.
# shellcheck disable=SC2015,SC2016  # SC2016: snippets expand in the child shell on purpose
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }
g() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main "$@"; }

# origin (bare) with four commits on main; the repository under test clones it.
g init -q --bare "$tmp/origin.git"
g init -q "$tmp/seed"
for i in 1 2 3 4; do g -C "$tmp/seed" commit -q --allow-empty -m "c$i"; done
g -C "$tmp/seed" push -q "$tmp/origin.git" HEAD:main
g clone -q "$tmp/origin.git" "$tmp/repo"
mkdir -p "$tmp/repo/deploy"
cp "$here/lib.sh" "$tmp/repo/deploy/lib.sh"
c() { git -C "$tmp/repo" rev-parse "origin/main~$((4 - $1))"; }   # c N: full SHA of commit cN

# Stub curl: answers .../commits/<sha>/check-runs from $STUB_DIR/<sha>.json; a missing file, or
# STUB_API_FAIL=<sha>, is an HTTP error (curl -f exits 22). Every requested SHA is logged.
mkdir -p "$tmp/bin" "$tmp/stub"
cat > "$tmp/bin/curl" <<'EOF'
#!/usr/bin/env bash
url="${*: -1}"; sha="${url%/check-runs*}"; sha="${sha##*/}"
echo "$sha" >> "$STUB_LOG"
[[ "${STUB_API_FAIL:-}" == "$sha" || ! -f "$STUB_DIR/$sha.json" ]] && { echo "curl: (22) 403" >&2; exit 22; }
cat "$STUB_DIR/$sha.json"
EOF
chmod +x "$tmp/bin/curl"

SIGN="publish (multi-arch index, cosign signature)"
# runs N JSON-RUN...: write the check-runs answer for commit cN.
runs() {
  local n="$1"; shift
  local IFS=,
  printf '{"total_count": %d, "check_runs": [%s]}' "$#" "$*" > "$tmp/stub/$(c "$n").json"
}
run() { printf '{"name": "%s", "status": "%s", "conclusion": %s}' "$1" "$2" "$3"; }
green=("$(run "cpp (release)" completed '"success"')" "$(run "publish (amd64)" completed '"success"')" \
  "$(run "$SIGN" completed '"success"')")

# pick [VAR=value...]: run latest_green_main; sets $rc and $out (stdout), $err (stderr).
pick() {
  : > "$tmp/log"
  out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" STUB_DIR="$tmp/stub" "$@" \
    bash -c "source '$tmp/repo/deploy/lib.sh'; latest_green_main" 2>"$tmp/err")"
  rc=$?
  err="$(cat "$tmp/err")"
}

echo "== newest commit green and published: chosen, one API call"
for n in 1 2 3 4; do runs "$n" "${green[@]}"; done
pick
[[ $rc -eq 0 && "$out" == "$(c 4)" ]] && ok "c4 chosen" || bad "rc=$rc out=$out err=$err"
[[ "$(wc -l < "$tmp/log")" -eq 1 ]] && ok "one API call" || bad "calls: $(wc -l < "$tmp/log")"

echo "== pending, failed, unpublished, empty: skipped"
runs 4 "${green[@]}" "$(run "go" in_progress null)"
runs 3 "${green[@]}" "$(run "web" completed '"failure"')"
runs 2 "$(run "cpp (release)" completed '"success"')" "$(run "$SIGN" completed '"skipped"')"
runs 1 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 1)" ]] && ok "c1 chosen" || bad "rc=$rc out=$out err=$err"
grep -q "checks still running: go" <<<"$err" && ok "pending reason logged" || bad "err: $err"
grep -q "checks failed: web" <<<"$err" && ok "failure reason logged" || bad "err: $err"
grep -q "not published and signed" <<<"$err" && ok "unpublished reason logged" || bad "err: $err"

echo "== sign check missing entirely: skipped"
runs 4 "$(run "cpp (release)" completed '"success"')"
runs 3 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 3)" ]] && ok "c3 chosen" || bad "rc=$rc out=$out err=$err"

echo "== no check runs at all: skipped"
runs 4
runs 3 "${green[@]}"
pick
[[ $rc -eq 0 && "$out" == "$(c 3)" ]] && ok "c3 chosen" || bad "rc=$rc out=$out err=$err"

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

echo "== nothing qualifies: error"
for n in 1 2 3 4; do runs "$n" "$(run "web" completed '"failure"')"; done
pick
[[ $rc -ne 0 && -z "$out" ]] && ok "exit non-zero" || bad "rc=$rc out=$out"
grep -q "none of the last 20 commits" <<<"$err" && ok "reason logged" || bad "err: $err"

echo "== is_rollback"
rb() { bash -c "source '$tmp/repo/deploy/lib.sh'; if is_rollback '$1' '$2'; then exit 0; else exit 1; fi"; }
rb "$(c 2)" "$(c 4 | cut -c1-7)" && ok "older than running: rollback" || bad "c2 vs c4 not a rollback"
rb "$(c 4)" "$(c 4 | cut -c1-7)" && bad "same commit called a rollback" || ok "same commit: not a rollback"
rb "$(c 4)" "$(c 2 | cut -c1-7)" && bad "newer commit called a rollback" || ok "newer: not a rollback"
rb "$(c 4)" "" && bad "empty TAG called a rollback" || ok "no running TAG: not a rollback"
rb "$(c 4)" "0000000" && bad "unknown TAG called a rollback" || ok "unknown running TAG: not a rollback"

if [[ $fails -gt 0 ]]; then
  echo "$fails check(s) failed" >&2
  exit 1
fi
echo "PASS: release.sh latest green"
