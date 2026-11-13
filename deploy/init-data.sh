#!/usr/bin/env bash
# One-time (idempotent) setup of the host data directories with the owners and modes the
# containers expect. Needs root for chown. On a host with a dedicated deploy user:
#
#   sudo RECS_OPS_OWNER=deploy deploy/init-data.sh
#
#   dir                 owner:group      mode  writer            readers
#   data/db             10001:10001      0700  api, backup       -
#   data/exports        10001:10003      2750  api               train (group; setgid keeps it)
#   data/posters        10001:10001      0700  api               -
#   data/backups        10001:10001      0700  backup            -
#   data/artifacts      10003:10000      2750  train             model (group recs)
#   data/reports        10003:10001      2750  train             api (group)
#   data/reports-private 10003:10003     0700  train             -
#   data/cache          10003:10003      0700  train             -
#   data/raw            unchanged              (owner's download) train (read-only mount)
#   data/ops/textfile   RECS_OPS_OWNER   0755  deploy scripts    node-exporter
#
# RECS_OPS_OWNER (a user name or UID) is the account that runs the deploy scripts: they write
# job status files into data/ops/textfile. It defaults to the user who ran sudo, so on a host
# where an admin account runs this but `deploy` runs the scripts, set it on every run, or a
# re-run hands data/ops back to the admin and the status files can no longer be written.
# Existing files inside these directories are re-owned to match (chown -R), so it is safe to
# run after a manual copy. data/raw is left alone; it must be world-readable (0755/0644).
set -Eeuo pipefail

DATA="${RECS_DATA_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data}"
[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0" >&2; exit 1; }

# Resolve the owner of data/ops before changing anything.
if [[ -n "${RECS_OPS_OWNER:-}" ]]; then
  if ! ops_uid="$(id -u -- "$RECS_OPS_OWNER" 2>/dev/null)" \
     || ! ops_gid="$(id -g -- "$RECS_OPS_OWNER" 2>/dev/null)"; then
    echo "RECS_OPS_OWNER: no such user '$RECS_OPS_OWNER'" >&2
    exit 1
  fi
else
  ops_uid="${SUDO_UID:-0}" ops_gid="${SUDO_GID:-0}"
fi

mk() { # mk DIR OWNER:GROUP MODE
  install -d -o "${2%%:*}" -g "${2##*:}" -m "$3" "$DATA/$1"
  chown -R "$2" "$DATA/$1"
  chmod "$3" "$DATA/$1"
  printf '  %-18s %s %s\n' "$1" "$2" "$3"
}

echo "data root: $DATA"
install -d -m 0755 "$DATA"
mk db               10001:10001 0700
mk exports          10001:10003 2750
mk posters          10001:10001 0700
mk backups          10001:10001 0700
mk artifacts        10003:10000 2750
mk reports          10003:10001 2750
mk reports-private  10003:10003 0700
mk cache            10003:10003 0700
install -d -m 0755 "$DATA/raw"
install -d -o "$ops_uid" -g "$ops_gid" -m 0755 "$DATA/ops" "$DATA/ops/textfile"
# Status files written before (for example by an earlier owner) move over too.
chown -R "$ops_uid:$ops_gid" "$DATA/ops"
echo "  ops/textfile       $ops_uid:$ops_gid 0755"
echo "done"
