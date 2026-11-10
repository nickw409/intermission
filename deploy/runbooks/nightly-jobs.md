# Nightly jobs and timers

`deploy/nightly.sh` runs at 03:30 from a systemd timer:

1. `recs-api export-ratings`: website ratings → `data/exports/` (internal user ids only).
2. `recs-train retrain` (only if the export succeeded): merge with MovieLens, train, evaluate,
   publish a new artifact and flip `CURRENT`; recs-model hot-swaps it.
3. `deploy/backup.sh`: `VACUUM INTO` + integrity check; 7 daily + 4 weekly copies.

Each step writes `data/ops/textfile/intermission_job_<step>.prom`, which node-exporter exposes
for the `NightlyJobFailed` and `NightlyJobStale` alerts. A monthly timer runs
`deploy/restore-drill.sh` (status file `intermission_job_restore_drill.prom`).

## Install the timers (once)

```bash
repo="$(pwd)"; user="$(id -un)"
for u in intermission-nightly.service intermission-nightly.timer \
         intermission-restore-drill.service intermission-restore-drill.timer; do
  sed -e "s|@REPO@|$repo|g" -e "s|@USER@|$user|g" "deploy/systemd/$u" | sudo tee "/etc/systemd/system/$u" >/dev/null
done
sudo systemctl daemon-reload
sudo systemctl enable --now intermission-nightly.timer intermission-restore-drill.timer
systemctl list-timers 'intermission-*'
```

## When an alert fires

```bash
journalctl -u intermission-nightly.service --since yesterday     # which step failed and why
```

- **export failed**: recs-api could not write `data/exports/` (check ownership:
  `sudo deploy/init-data.sh`) or the database is locked/corrupt (`restore.md`). Retrain was
  skipped; the site keeps serving the current model.
- **retrain failed**: recs-model keeps serving the previous artifact (a half-written artifact
  is never published). Check the train output in the journal; out-of-memory shows as exit 137
  (raise `TRAIN_MEM_LIMIT` in `deploy/.env` if the host has room).
- **backup failed**: the integrity check failed or the disk is full. Run `deploy/backup.sh` by
  hand; if the integrity check fails on a fresh copy, the live database may be damaged: stop
  and investigate before the good backups age out (7 days).
- **stale (36 h without success)**: the timer did not run (machine asleep, timer disabled):
  `systemctl list-timers 'intermission-*'`.

Re-run everything by hand: `sudo systemctl start intermission-nightly.service`, or the steps
individually (`deploy/backup.sh`; `docker compose -f deploy/compose.yaml run --rm train retrain`).
All steps are safe to re-run.
