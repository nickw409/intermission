# Restore the database from a backup

Backups: `data/backups/daily/app-YYYY-MM-DD.db` (7 kept) and `data/backups/weekly/` (4 kept),
made nightly by `deploy/backup.sh` with `VACUUM INTO` and checked with `PRAGMA integrity_check`.
Files are 0600, owned by UID 10001. Model artifacts are not backed up (retraining rebuilds them).

## 1. Rehearse first (production keeps running)

```bash
deploy/restore-drill.sh
```

restores the newest backup into a scratch compose project on `127.0.0.1:18080` and runs the
smoke test against it. It runs monthly from a timer; run it now to confirm the backup you are
about to use is good.

## 2. Restore production

First stop the timers, so the nightly backup or export can't run against a half-restored
database, and wait for any job that is already running:

```bash
sudo systemctl stop intermission-nightly.timer intermission-restore-drill.timer
systemctl is-active intermission-nightly.service intermission-restore-drill.service   # both "inactive"
cat deploy/ops.lock 2>/dev/null; flock -n deploy/ops.lock true && echo "lock free"     # no script running
```

Then restore:

```bash
c="docker compose -f deploy/compose.yaml"
$c run --rm --no-deps backup latest --dir /srv/data/backups      # newest; or pick a file
$c run --rm --no-deps backup verify --file /srv/data/backups/daily/app-2026-11-09.db
# optional: stop public traffic first (take-offline.md)
$c stop api                                                       # nothing may write app.db
$c run --rm --no-deps backup backup --db /srv/data/db/app.db --dir /srv/data/backups/pre-restore
$c run --rm --no-deps backup restore --force \
    --from /srv/data/backups/daily/app-2026-11-09.db --db /srv/data/db/app.db
deploy/release.sh "$(sed -n 's/^TAG=//p' deploy/.env)"            # start api again + smoke test
deploy/backup.sh                                                  # first backup of the restored db
sudo systemctl start intermission-nightly.timer intermission-restore-drill.timer
```

- `restore` re-checks the backup's integrity, copies it into place atomically, removes the old
  `-wal`/`-shm` files (they belong to the replaced database) and checks the result.
- The extra backup into `pre-restore/` keeps the state you are replacing, in case the restore
  was the wrong call. Delete it once you are sure.
- Everything written after the backup was taken (ratings, signups) is lost. Sessions created
  since then are invalid; users just log in again.
