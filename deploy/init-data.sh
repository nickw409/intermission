#!/usr/bin/env bash
# One-time (idempotent) setup of the host data directories with the owners and modes the
# containers expect. Needs root for chown: run it as `sudo deploy/init-data.sh`.
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
#   data/ops/textfile   invoking user    0755  deploy scripts    node-exporter
#
# Existing files inside these directories are re-owned to match (chown -R), so it is safe to
# run after a manual copy. data/raw is left alone; it must be world-readable (0755/0644).
set -Eeuo pipefail

DATA="${RECS_DATA_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data}"
[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0" >&2; exit 1; }

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
owner="${SUDO_UID:-0}:${SUDO_GID:-0}"
install -d -o "${owner%%:*}" -g "${owner##*:}" -m 0755 "$DATA/ops" "$DATA/ops/textfile"
echo "  ops/textfile       $owner 0755"
echo "done"
