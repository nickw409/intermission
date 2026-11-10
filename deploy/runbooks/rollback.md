# Rollback

Use when a deploy passed its smoke test but misbehaves later (alerts `SiteDown`,
`High5xxRate`, user reports).

```bash
deploy/release.sh --list          # last 5 deployed SHAs, oldest first; the last line is live
deploy/release.sh <previous-sha>  # same command as a deploy: up, /healthz, smoke test
```

- The previous images are still on the host (nothing prunes `intermission-*` images).
  If they were removed, `release.sh` rebuilds them from git (~10 min).
- Database: migrations only add, so the older recs-api runs against the newer schema. Never
  restore a backup just to roll back code. If a migration itself is broken, see `restore.md`.
- Model artifacts are independent of the image tag: recs-model loads whatever `CURRENT`
  points to. To roll back a bad model, point `data/artifacts/CURRENT` at the previous version
  directory (write `CURRENT.tmp`, then `mv` it over `CURRENT`); recs-model hot-swaps within
  seconds and keeps serving the old model if verification fails.

If the rollback's smoke test fails too, the site is degraded: take it offline
(`take-offline.md`), then debug with `docker compose -f deploy/compose.yaml logs --tail 200`.
