#!/usr/bin/env bash
# Tests for deploy/nightly.sh with docker stubbed:
#   - default (RECS_NIGHTLY_RETRAIN unset or `local`): export, retrain, backup, in that order
#   - RECS_NIGHTLY_RETRAIN=off (a serving-only host; training runs elsewhere): export and backup
#     run, retrain is never started, its stale status file is removed so an old failure cannot
#     keep JobFailed firing, and the run succeeds
#   - an unknown value is refused before any job starts
#   - a failed export still skips retrain and still runs the backup (unchanged behaviour)
#
#   deploy/tests/test_nightly.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else.
# shellcheck disable=SC2015,SC2016  # SC2016: the stub is written single-quoted on purpose
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }

# Stub docker: logs the compose service and command; STUB_FAIL=<service> makes it fail.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/docker" <<'EOF'
#!/usr/bin/env bash
args=" $* "
for svc in api train backup; do
  if [[ "$args" == *" run --rm --no-deps $svc"* || "$args" == *" run --rm --no-deps $svc "* ]]; then
    rest="${args#* run --rm --no-deps $svc}"
    echo "$svc${rest% }" >> "$STUB_LOG"
    [[ "${STUB_FAIL:-}" == "$svc" ]] && exit 1
    exit 0
  fi
done
echo "unexpected: $*" >> "$STUB_LOG"
EOF
chmod +x "$tmp/bin/docker"

data="$tmp/data"
status_dir="$data/ops/textfile"
for d in exports artifacts reports reports-private cache raw db backups ops/textfile; do mkdir -p "$data/$d"; done

# run_nightly ENV_LINE [VAR=value...]: one nightly run; sets $rc and $out, calls in $tmp/log.
run_nightly() {
  local line="$1"; shift
  printf 'TAG=0000000\nRECS_SITE_ORIGIN=http://localhost:8080\nSMOKE_PASSWORD=real-value\nGRAFANA_ADMIN_PASSWORD=real-value\n%s\n' \
    "$line" > "$tmp/env"
  chmod 600 "$tmp/env"
  : > "$tmp/log"
  out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" RECS_ENV_FILE="$tmp/env" \
    RECS_LOCK_FILE="$tmp/ops.lock" RECS_DATA_DIR="$data" "$@" "$here/nightly.sh" 2>&1)"
  rc=$?
}
calls() { tr '\n' '|' < "$tmp/log"; }
status_of() { sed -n "s/^intermission_job_last_status{task=\"$1\"} //p" "$status_dir/intermission_job_$1.prom" 2>/dev/null; }

echo "== default: export, retrain, backup"
for line in "" "RECS_NIGHTLY_RETRAIN=local"; do
  run_nightly "$line"
  [[ $rc -eq 0 ]] && ok "'$line': exit 0" || bad "'$line': exit $rc: $out"
  [[ "$(calls)" == "api export-ratings|train retrain|backup|" ]] && ok "'$line': all three steps in order" \
    || bad "'$line': calls $(calls)"
done

echo "== off: retrain never started, stale status removed"
printf 'intermission_job_last_status{task="retrain"} 1\n' > "$status_dir/intermission_job_retrain.prom"
run_nightly "RECS_NIGHTLY_RETRAIN=off"
[[ $rc -eq 0 ]] && ok "exit 0" || bad "exit $rc: $out"
[[ "$(calls)" == "api export-ratings|backup|" ]] && ok "export and backup only" || bad "calls $(calls)"
[[ ! -e "$status_dir/intermission_job_retrain.prom" ]] && ok "stale retrain status removed" || bad "retrain status still there"
[[ "$(status_of export) $(status_of backup)" == "0 0" ]] && ok "export and backup recorded OK" \
  || bad "export=$(status_of export) backup=$(status_of backup)"
grep -q "retrain.*off" <<<"$out" && ok "skip is logged" || bad "skip not logged: $out"

echo "== off and a failed export: backup still runs, run fails, no retrain status"
run_nightly "RECS_NIGHTLY_RETRAIN=off" STUB_FAIL=api
[[ $rc -ne 0 ]] && ok "exit non-zero" || bad "exit 0"
[[ "$(calls)" == "api export-ratings|backup|" ]] && ok "backup still ran" || bad "calls $(calls)"
[[ ! -e "$status_dir/intermission_job_retrain.prom" ]] && ok "no retrain status written" || bad "retrain status written"

echo "== default and a failed export: retrain skipped and marked failed (unchanged)"
run_nightly "" STUB_FAIL=api
[[ $rc -ne 0 ]] && ok "exit non-zero" || bad "exit 0"
[[ "$(calls)" == "api export-ratings|backup|" ]] && ok "retrain skipped, backup ran" || bad "calls $(calls)"
[[ "$(status_of retrain)" == 1 ]] && ok "retrain status 1" || bad "retrain status '$(status_of retrain)'"

echo "== unknown value: refused before any job"
run_nightly "RECS_NIGHTLY_RETRAIN=remote"
[[ $rc -ne 0 ]] && ok "exit non-zero" || bad "exit 0"
[[ -z "$(calls)" ]] && ok "no job started" || bad "calls $(calls)"
grep -q RECS_NIGHTLY_RETRAIN <<<"$out" && ok "error names the setting" || bad "error: $out"

if [[ $fails -gt 0 ]]; then
  echo "$fails check(s) failed" >&2
  exit 1
fi
echo "PASS: nightly.sh"
