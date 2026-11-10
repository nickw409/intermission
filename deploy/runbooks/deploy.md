# Deploy

Every production change is one image set, tagged with a short git SHA, deployed by one command.
Commands run from the repo root on the host, as the deploy user (a member of the `docker` group).

## Routine deploy

```bash
git fetch && git log --oneline -3 origin/main     # pick the SHA (CI must be green for it)
deploy/release.sh <sha>
```

`release.sh`:

1. builds `intermission-{api,model,train,web,backup}:<sha>` from a clean `git worktree` of the
   commit if they are not already loaded (`deploy/build.sh <sha>`; ~10 min cold, C++ dominates),
2. writes `TAG=<sha>` to `deploy/.env`,
3. `docker compose up -d caddy api model`, then checks that all three run the new tag,
4. waits up to 120 s for `/healthz` through Caddy,
5. runs `deploy/smoke.sh` (healthz, SPA headers, login as the smoke account, `/api/me`,
   `/api/recommend`, logout),
6. records the SHA in `deploy/history` (last 5).

If any step fails it prints the last 50 log lines and **rolls back automatically** to the
previous `TAG` (pass `--no-rollback` to keep the failed version up for debugging). Exit code is
non-zero either way. Re-running the same SHA is safe: it just re-checks it.

Database migrations run automatically when recs-api starts and must work with both the old
and new recs-api (add columns, never rename or drop in the same release; spec "Build and
deploy"). recs-api takes its own backup before migrating.

Using CI-built images instead of building locally: download the `images-<sha>` artifact from
the `release` workflow run, then `zstd -dc images-<sha>.tar.zst | docker load` and run
`deploy/release.sh <sha>`.

## First-time setup (once per machine)

1. Install Docker Engine (with the compose plugin), `curl`, `python3` and `git`. Tailscale is
   installed on the host, not in a container (see `funnel-setup.md`; owner only).
2. Clone the repo. Production data lives in `data/` (gitignored). To keep local development
   runs from touching it, you can point production elsewhere with `RECS_DATA_DIR=/srv/intermission/data`
   in `deploy/.env`.
3. `sudo deploy/init-data.sh` creates the data directories with the owners the containers use
   (UIDs 10001–10004, group 10000). Idempotent.
4. `install -m 0600 deploy/.env.example deploy/.env`, then edit it: `RECS_SITE_ORIGIN`
   (`https://intermission.<tailnet-name>.ts.net`), `SMOKE_PASSWORD` (long, random),
   `GRAFANA_ADMIN_PASSWORD`. The TMDB key is added by the owner (`rotate-tmdb-key.md`).
5. Get MovieLens into `data/raw/` (`tools/download_movielens.sh ml-32m`), build the images
   (`deploy/build.sh <sha>`), set `TAG=<sha>` in `deploy/.env`, then build the first artifact:

   ```bash
   docker compose -f deploy/compose.yaml run --rm train ingest --dataset ml-32m
   docker compose -f deploy/compose.yaml run --rm train train --dataset ml-32m ...   # see train/README
   ```

6. Create the smoke-test account (needs an invite; `create-invites.md`):

   ```bash
   code=$(docker compose -f deploy/compose.yaml run --rm --no-deps api admin invite create --uses 1 --expires 1d --note smoke)
   # then sign up as SMOKE_USERNAME with that code through the site (or curl /api/auth/signup
   # with headers Origin: <RECS_SITE_ORIGIN> and X-Requested-With: recs)
   ```

7. `deploy/release.sh <sha>`.
8. Install the timers (`nightly-jobs.md`) and, optionally, monitoring (`monitoring.md`).

## Checks after a deploy

- `deploy/release.sh --list` shows the new SHA at the bottom.
- `docker compose -f deploy/compose.yaml ps`: caddy, api, model are `running`.
- `docker compose -f deploy/compose.yaml logs --since 10m api | grep -c '"level":"ERROR"'` is 0.
