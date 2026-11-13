#!/usr/bin/env bash
# Tests for deploy/init-data.sh, run as root in a throwaway container (it needs root to chown):
#   - default: data/ops belongs to the user who ran sudo (SUDO_UID/SUDO_GID)
#   - RECS_OPS_OWNER=<user> gives data/ops (and files already in it) to that user, so a re-run
#     with sudo from an admin account no longer takes it away from the deploy user
#   - an unknown RECS_OPS_OWNER is refused before anything is created or changed
#   - the container directories keep their fixed owners and modes
#
#   deploy/tests/test_init_data.sh
set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The Ubuntu image the C++ Dockerfiles already pin (multi-arch), so no new dependency.
image="ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852"

docker run --rm --network none -v "$here:/deploy:ro" "$image" bash -c '
set -uo pipefail
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }
owner() { stat -c "%U:%G" "$1"; }
useradd -m admin && useradd -m deploy

echo "== default: the sudo user owns ops"
D=/tmp/d1
RECS_DATA_DIR=$D SUDO_UID=$(id -u admin) SUDO_GID=$(id -g admin) /deploy/init-data.sh >/dev/null
[[ "$(owner $D/ops/textfile)" == admin:admin ]] && ok "ops/textfile admin:admin" || bad "ops/textfile $(owner $D/ops/textfile)"

echo "== RECS_OPS_OWNER=deploy, re-run by the admin: deploy keeps ops"
touch $D/ops/textfile/intermission_job_backup.prom; chown admin:admin $D/ops/textfile/intermission_job_backup.prom
RECS_DATA_DIR=$D RECS_OPS_OWNER=deploy SUDO_UID=$(id -u admin) SUDO_GID=$(id -g admin) /deploy/init-data.sh >/dev/null \
  && ok "exit 0" || bad "exit non-zero"
for p in ops ops/textfile ops/textfile/intermission_job_backup.prom; do
  [[ "$(owner $D/$p)" == deploy:deploy ]] && ok "$p deploy:deploy" || bad "$p $(owner $D/$p)"
done
[[ "$(stat -c %a $D/ops/textfile)" == 755 ]] && ok "ops/textfile 0755" || bad "ops/textfile mode $(stat -c %a $D/ops/textfile)"
su deploy -s /bin/bash -c "touch $D/ops/textfile/.w" && ok "deploy can write ops/textfile" || bad "deploy cannot write"

echo "== numeric RECS_OPS_OWNER"
D=/tmp/d2
RECS_DATA_DIR=$D RECS_OPS_OWNER=$(id -u deploy) /deploy/init-data.sh >/dev/null
[[ "$(owner $D/ops/textfile)" == deploy:deploy ]] && ok "uid accepted" || bad "ops/textfile $(owner $D/ops/textfile)"

echo "== unknown RECS_OPS_OWNER: refused, nothing created"
D=/tmp/d3
if out=$(RECS_DATA_DIR=$D RECS_OPS_OWNER=nosuchuser /deploy/init-data.sh 2>&1); then bad "exit 0"; else ok "exit non-zero"; fi
grep -q nosuchuser <<<"$out" && ok "error names the user" || bad "error: $out"
[[ ! -e $D ]] && ok "nothing created" || bad "created $D"

echo "== container directories unchanged"
D=/tmp/d1
while read -r dir want mode; do
  got="$(stat -c "%u:%g %a" $D/$dir)"
  [[ "$got" == "$want $mode" ]] && ok "$dir $got" || bad "$dir $got (want $want $mode)"
done <<EOF
db 10001:10001 700
exports 10001:10003 2750
posters 10001:10001 700
backups 10001:10001 700
artifacts 10003:10000 2750
reports 10003:10001 2750
reports-private 10003:10003 700
cache 10003:10003 700
EOF

[[ $fails -eq 0 ]] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: init-data.sh"
'
