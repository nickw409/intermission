# Automated deploy (follow-up plan)

Status: **step 0 done**: merged in PR #68 (`ca223ad`, 2026-11-12) but not deployed yet (the next `release.sh` deploys it). Steps 1–4 have not started.

Today a release reaches the site only when someone runs `deploy/release.sh <sha>` on the host.
CI builds and checks images on every push to `main` but cannot deploy (no registry, no access to
the host). The goal is continuous deployment of green `main` commits, with releases that need a
retrain handled safely.

## Step 0: retrain detection (done, PR #68)

- `recs-model --check` loads and verifies the artifact `CURRENT` names through the server's own
  watcher code, prints `version=`, `git_sha=`, `models=`, and exits 0 or 1.
- `release.sh` runs it with the new image before changing anything and stops if it fails
  (`RetrainRequired`, task `artifact_check`). After a successful release it diffs `core/`,
  `train/` and `configs/` against the artifact's `git_sha` (`RetrainRecommended`, task
  `artifact_freshness`).
- Runbook: `deploy/runbooks/deploy.md`, "Retrain detection".
- Known limits, to address here: rollbacks to images older than `--check` skip it; the check has
  no timeout (it holds `ops.lock` if it ever hangs; it takes about 1 s today); manual training
  runs do not take `ops.lock`; the full `release.sh` flow has no automated test (only its
  `lib.sh` pieces, with `compose` stubbed).

## Step 1: alert delivery (prerequisite, owner decision)

Nothing pushes alerts to the owner yet (`deploy/runbooks/monitoring.md`, "Known gaps"; audit
follow-up SA6-2). Unattended deploys are not safe until a blocked or failed one reaches a person.

- Add Alertmanager to the `monitoring` profile with one receiver. Proposed: ntfy push (a long
  random topic in `deploy/.env`, or self-hosted ntfy). Alternatives: SMTP email, Discord webhook.
- Owner decisions: the channel, and allowing an outbound connection from the host for it (the
  project's network rule allows only the TMDB poster fetch at runtime).
- Send a test alert as the acceptance check.

## Step 2: auto-deploy trigger

- A systemd timer (for example every 10 minutes) runs `deploy/auto-release.sh`:
  1. `git fetch`; take `origin/main`; skip if it equals the running `TAG`.
  2. Require that the commit's required checks passed (`gh api repos/.../commits/<sha>/check-runs`,
     which needs a read-only token on the host; or a self-hosted runner that calls the script after
     CI, which needs none but runs CI code on the host: decide which).
  3. Skip if `data/ops/auto-release.paused` exists.
  4. `timeout` around `deploy/release.sh <sha>` and around the check. On any failure (blocked preflight, failed smoke test, rollback),
     create the pause file with the reason and record `job_status release 1`.
- New alert `ReleaseFailed` on task `release`; `RetrainRequired` already covers the blocked case.
- The owner clears the pause file after dealing with it (`deploy/runbooks/deploy.md` section).
- Optional: use the CI-built image artifact instead of building locally (saves ~10 min).

## Step 3: automatic retrain when required

When the preflight fails, retrain with the new image and release, in an order that never leaves
recs-model without a loadable artifact (deploy runbook warning about switching `CURRENT` early):

1. Build the new images.
2. Train with the new `train` image **without** `--set-current`.
3. Check the new artifact with the new recs-model: needs `--check --version <v>` (check a named
   version instead of `CURRENT`; small addition to `model-service/src/check.cpp`).
4. Switch `CURRENT` and run `release.sh` straight away. The old recs-model also watches `CURRENT`,
   rejects the new format and keeps serving until it is replaced.
5. If the release rolls back, point `CURRENT` back at the previous artifact too
   (`release.sh --artifact <version>` could own both).

Open questions:
- Training time: the ml-32m production train took 37 min (2026-11-12). MF dominates and has no
  per-model timing yet; measure it before deciding whether a deploy may wait that long.
- Memory: train (`mem_limit` 12g) plus the serving model (4g) plus the check (4g) on a 32 GB host.
- Lock: the train step holds `ops.lock`, so a nightly job can wait up to the training time.

## Step 4: retrain when recommended

`RetrainRecommended` does not block. Fold it into the nightly loop once `recs-train retrain`
(still a stub) is implemented: retrain when the flag is set, then re-run the check.
