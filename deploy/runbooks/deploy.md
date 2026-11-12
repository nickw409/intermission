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
2. runs the new image's `recs-model --check` against the artifact `CURRENT` points at (see
   "Retrain detection" below) and **stops, changing nothing,** if it would not load or the check
   cannot run,
3. writes `TAG=<sha>` to `deploy/.env`,
4. `docker compose up -d caddy api model`, then checks that all three run the new tag,
5. waits up to 120 s for `/healthz` through Caddy,
6. runs `deploy/smoke.sh` (healthz, SPA headers, login as the smoke account, `/api/me`,
   `/api/recommend`, logout),
7. records the SHA in `deploy/history` (last 5),
8. notes whether the serving artifact was trained by older training code (see below).

If a step after the check fails it prints the last 50 log lines and **rolls back automatically** to the
previous `TAG` (pass `--no-rollback` to keep the failed version up for debugging). Exit code is
non-zero either way. Re-running the same SHA is safe: it just re-checks it.

Database migrations run automatically when recs-api starts and must work with both the old
and new recs-api (add columns, never rename or drop in the same release; spec "Build and
deploy"). recs-api takes its own backup before migrating.

Using CI-built images instead of building locally: download the `images-<sha>` artifact from
the `release` workflow run, then `zstd -dc images-<sha>.tar.zst | docker load`, `git fetch`
(release.sh only accepts SHAs that exist in the local repository) and run
`deploy/release.sh <sha>`.

Before any of this, the scripts check `deploy/.env`: it is forced to mode 0600, and they stop if
any value still starts with `change-me` (the `.env.example` placeholders). Release, backup,
nightly and restore-drill also share one lock (`deploy/ops.lock`), so they never overlap; a
release waits at most 2 minutes for it (`RELEASE_LOCK_WAIT`), then stops and names the holder.

Development uses a separate compose project (`intermission-dev`), port 127.0.0.1:8088 and data
directory `data/dev/` (see `deploy/compose.dev.yaml`), so it never touches production.

### Retrain detection

`release.sh` answers "does this release need a retrain?" in two parts:

- **Required (the release stops).** Before touching anything it runs the new image's
  `recs-model --check` as the one-off compose service `model-check` (`deploy/lib.sh`
  `check_artifact`): the model image, user and artifacts (read-only), no network and no model
  socket. It loads and verifies the artifact `CURRENT` names through the same code the server
  uses, and exits:
  - **0**: it loads; the release continues.
  - **3**: that recs-model would refuse it (a model file format it no longer reads, or, on a
    rollback, one newer than it knows). The release stops, the running version keeps serving, and
    `RetrainRequired` fires (`intermission_job_last_status{task="artifact_check"}`). Retrain with
    the new train image as below, then run `release.sh <sha>` again. A later passing check, or
    any later successful release, clears the alert.
  - **4**: there is no `CURRENT`. The release stops; train a first artifact (first-time setup).
  - anything else, or the image does not start: the check could not run. The release stops with
    the reason; no alert is recorded.
- **Recommended (the release goes ahead).** The check prints the artifact's `git_sha` (the
  commit whose train image built it). After a successful release, `release.sh` diffs the training
  code between that commit and the release (`TRAINING_PATHS` in `lib.sh`: `core/`, `train/`,
  `configs/`, `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `train.Dockerfile`). Any
  difference, in either direction (a rollback can leave the artifact newer than the code), logs a
  note and fires `RetrainRecommended` (severity info, `task="artifact_freshness"`). It is
  deliberately broad: a comment edit in `core/` also counts. After retraining, re-run
  `deploy/release.sh <running sha>` (quick: images are present) to re-check and clear it. An
  artifact whose `git_sha` is not exactly one commit in this repository is reported as "cannot
  tell" and records nothing.

**Images built before `--check` existed** (every release up to and including `3cdeb1d`) cannot
be asked. For a rollback to one of them, the preflight and the freshness check are skipped with a
warning, so the protection against an artifact newer than the code (the format 3 KNN case below)
does not apply: that case still fails at the smoke test and rolls back automatically. Only a
target image that has `--check` is checked.

Run training (`compose run --rm train train ... --set-current`) and `release.sh` one after the
other, never at the same time: the training run does not take `ops.lock`, so `CURRENT` could
change between the check and `compose up`.

The check loads the full artifact next to the running recs-model, so it briefly needs as much
memory again (`MODEL_MEM_LIMIT`, 4 GB by default). On the production artifact (ml-32m, six
models) it took about a second. Alerts show at http://127.0.0.1:9090/alerts until a delivery
channel exists (`monitoring.md`, "Known gaps").

### Model artifacts that need a retrain first

 A content model saved before the profile-centering
change (Phase 1, `docs/phase-1.md` "Content centering (option b)") has no `content/centering.bin`.
The new recs-model refuses it ("content artifact predates centering.bin; retrain"), and because an
artifact loads as one unit, the whole artifact would be rejected. Before deploying that change (or
any later SHA), train a new artifact with the new image (below) so `CURRENT` points at an
artifact that has the file. Production has no such artifact yet, but check `data/artifacts/CURRENT` first.

The MF model file went to format 2 with the min_support ranking floor (it now stores each
movie's training rating count). recs-model refuses a format 1 `mf/item_factors.bin` ("format 1
has no per-item rating counts (min_support); retrain"), which again rejects the whole artifact.
Order for that release: `deploy/build.sh <sha>`, then a new artifact from the new image (the
running recs-model keeps serving the artifact it already loaded):

```bash
TAG=<sha> docker compose -f deploy/compose.yaml run --rm train train --dataset ml-32m \
  --models pop,pop_count,knn,content,mf,als --default-model knn --set-current --seed 42
```

then `deploy/release.sh <sha>`, which starts the new recs-model on the new artifact. (`recs-train
retrain`, the nightly merge-and-train step, is still a stub that exits with "retrain is not
implemented yet", so use `train` as above.) Run the release straight after the training: if
the old recs-model restarts in between, it finds only the format 2 artifact in `CURRENT` and
cannot load it, so recommendations are down until the release. Point `CURRENT` back at the
previous artifact if the release has to wait.

The KNN model file went to format 3 with `mean_damping` (it stores the damping and the global
mean). The new recs-model still loads format 2 KNN files (as `mean_damping` 0, the old
behaviour), so the release itself needs no retrain; the damped mean reaches the site with the
next artifact trained by the new image. Rollback in the other direction does not work: an older
recs-model refuses a format 3 `knn/item_knn.bin` ("unsupported format version"). Once a format 3
artifact is `CURRENT`, rolling the code back to a pre-format-3 SHA also needs `CURRENT` pointed
at an older artifact (keep the previous artifact until the new one has been live for a while).

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
   docker compose -f deploy/compose.yaml run --rm train train --dataset ml-32m \
     --models pop,pop_count,knn,content,mf,als --default-model knn --set-current --seed 42
   ```

   The train job mounts `configs/` read-only (`RECS_CONFIG_DIR`). Each model uses
   `configs/<model>.<dataset>.tuned.json` when it exists (for example
   `knn.ml-32m.tuned.json`), else `configs/<model>.json`, else its built-in defaults; the
   train log prints which file each model used (`config <model>: <path>`).

6. Create the smoke-test account (needs an invite; `create-invites.md`):

   ```bash
   # admin never migrates; on a brand-new database apply the schema first:
   docker compose -f deploy/compose.yaml run --rm --no-deps api migrate
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

## Accepted risk: recs-api is reachable from the host itself

Only Caddy publishes a port, and only on 127.0.0.1. But Docker routes every bridge network to
the host, **including networks marked `internal`** (verified on the build host: a container
on an internal network answered a TCP connection from the host). So any process
running on the host can connect to recs-api's container address (on `backend` or `egress`,
port 8081) and skip Caddy: no security headers, no 64 KB body cap, no access log line.
recs-api's own checks (auth, CSRF, body limits, rate limits) still apply, and nothing on the
internet or the tailnet can reach those addresses (Funnel forwards only to 127.0.0.1:8080).

Decision: **accepted as a local-only risk.** Anyone who can run processes on the host can
already reach the Docker socket or the data directory, which is strictly more access. An egress
proxy for the TMDB fetch was considered and rejected: it would take api off the `egress`
bridge, but the host could still reach it on the internal `backend` bridge, so it would add an
image and its config without closing the path. Revisit this if the host ever runs untrusted
local users or workloads; the fix then is host firewall rules (the `DOCKER-USER` chain)
dropping host-originated traffic to the bridge subnets.
