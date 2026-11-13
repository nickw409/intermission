# Move production to a Raspberry Pi 4 — plan

Status: **in progress.** Hardware chosen by the owner (2026-11-12). §2.1 and §2.3 are done (PR
#105); §2.2 is #97 (PR #106). The top-level `CMakeLists.txt` and `.github/workflows/ci.yml` are
frozen interfaces: their changes in #105 had an independent review in place of the orchestrator.

Related: #97 (publish images to GHCR), #98 (production server requirements), the
automated-deploy plan (`2026-11-12-automated-deploy.md`, #77).

**Decisions so far (owner, 2026-11-12)**

| Question | Decision |
|---|---|
| Production host | Raspberry Pi 4, 4 GB, at home, published with Tailscale Funnel as today |
| Training host | The desktop (the current dev host). The Pi only serves. |
| Artifacts desktop → Pi | Approved |
| Ratings export Pi → desktop (website users' ratings in training) | Approved |

## 1. Why it fits: measurements

Measured 2026-11-12 on the desktop (x86) with image set `6c79854` and ml-32m artifacts for all six
models. Raw logs are not kept in the repo.

| | Memory | Notes |
|---|---|---|
| Caddy | ~13 MiB | |
| recs-api | ~135 MiB, plus up to 256 MiB in login bursts | Argon2id m=64 MiB, at most 4 hashes at once |
| recs-model, 6 ml-32m models | 131 MiB idle, 162 MiB peak under load | Artifact 75 MB on disk; loads in < 1 s |
| **Serving total** | **~0.3 GB typical, ~0.6 GB worst case** | Fits a 4 GB Pi with room for the OS, Docker, Tailscale |
| Training (all six models) | 2.51 GiB peak, 41 min on 4 cores | MF 38.5 min; the rest ~2.5 min |
| Eval | 3.5–4.2 GB peak | `data/reports/*.json` |

recs-model pinned to 2 cores, 20 requests always in flight, 20 ratings per request:

| Endpoint | Req/s | p99 |
|---|---|---|
| knn | 1,080 | 40 ms |
| content | 495 | 53 ms |
| als | 385 | 124 ms |
| mf | 322 | 111 ms |
| compare (3 models) | 264 | 138 ms |

The Pi 4's Cortex-A72 cores are expected to be roughly 2–3× slower per core. That is an estimate:
§5 measures it. Training does not fit alongside serving on 4 GB (eval does not fit at all), so it
stays on the desktop.

## 2. Code and CI

### 2.1 Disable FMA contraction (done, PR #105)

GCC contracts `a*b + c` into fused multiply-adds by default on arm64, and no build file set
`-ffp-contract=off`. `core/src/models/mf_sgd.cpp` (header comment) relies on no contraction for
bit-identical training, and the golden tests pin exact values. Measured on the native arm64
runner before the fix: 2 of 329 tests failed, both bit-exact (knn gate 1, the MF bit-for-bit
golden test); with the flag all pass.

- `-ffp-contract=off` is now a project-wide compile option (top-level `CMakeLists.txt`), ahead of
  the third-party dependencies, so it covers every target and the image builds.
- `CMakeLists.txt` is in `TRAINING_PATHS` (`deploy/lib.sh`), so the next release reports
  `RetrainRecommended` although x86 output is unchanged. Expected; a retrain clears it.
- Gate: the existing golden and determinism tests pass on arm64 (§2.3).

### 2.2 Multi-arch images (amends #97)

- Build `linux/amd64` and `linux/arm64` on native runners (GitHub's arm64 runners for public
  repos), not under QEMU: the C++ build is too slow emulated. Merge the two builds into one
  manifest list per image and sign that (#97's cosign step).
- Verify every pinned base-image digest is a manifest list that includes arm64 (`docker buildx
  imagetools inspect <ref>@<digest>`). Re-pin any that are single-arch.
- Amend #97's done-when "The CI arch is stated (amd64 only)" to "amd64 and arm64".
- `deploy/fetch-images.sh` pulls the host's platform, and `deploy/build.sh` (the fallback) builds
  natively on the Pi. That works but is slow; measure it once.

### 2.3 arm64 test job

Done (PR #105): `cpp (release, arm64)` runs the release C++ suite on a native arm64 runner on
every push and PR. This is what proves §2.1. The Go services are pure Go and are exercised on
arm64 by #97's `publish (arm64)` job (image build plus the edge gate), not by a separate test job.

### 2.4 Compose and `.env` for the Pi

- `MODEL_MEM_LIMIT=1g` (162 MiB measured peak); keep api at 768m and caddy at 256m.
- `MODEL_OMP_THREADS`: the Pi has 4 cores; start at 2 and confirm in §5.
- Remove `NODE_EXPORTER_DATA_PROPAGATION=` (a WSL2-only workaround).
- The monitoring profile (Prometheus 1g + Grafana 512m limits) fits on 4 GB but is the first thing
  to move off the Pi if memory gets tight.
- `train` and `model-check` keep their profiles; `train` is not run on the Pi.

## 3. Pi setup (owner)

Hardware:
- Pi 4, 4 GB, booting from a **USB 3 SSD**, not microSD (SQLite and log writes wear SD cards).
- Active cooling (the A72 throttles under sustained load), official power supply, wired Ethernet.

OS and services:
- 64-bit OS: Raspberry Pi OS Lite (64-bit) or Ubuntu Server 24.04 arm64.
- **Memory cgroup.** Raspberry Pi OS has shipped with it disabled, which makes Docker ignore every
  `mem_limit`. Add `cgroup_enable=memory cgroup_memory=1` to `/boot/firmware/cmdline.txt`, reboot,
  and confirm `docker info` prints no memory-limit warning.
- Docker Engine, Tailscale, unattended-upgrades, time sync.
- #98 requirements: a dedicated `deploy` user (docker group only), no GitHub credentials, SSH over
  Tailscale only, outbound allowlist (`github.com`, `api.github.com`, `ghcr.io`, the TMDB API, the
  alert channel).
- `deploy/init-data.sh` for the data directories, then the systemd units in `deploy/systemd/`.

## 4. Training on the desktop

- The desktop keeps `data/raw` and `data/cache`. The Pi needs neither, and holds no MovieLens raw
  data at all (rule 10 still applies to both machines).
- **Artifacts desktop → Pi (approved).** The desktop trains, then copies the artifact directory to
  the Pi over Tailscale into a staging path. The Pi verifies it with the new `recs-model --check
  --version <v>` (automated-deploy plan step 3) before switching `CURRENT`. The transfers need a
  separate account on the Pi (not the `deploy` user) that can write only the staging directory
  and read only the exports directory.
- **Ratings export Pi → desktop (approved).** The spec appends website users' ratings at retrain
  time, so the nightly export (personal data) leaves the Pi for the desktop:
  - the desktop pulls it over Tailscale only, with the transfer account above;
  - it lands in the desktop's gitignored `data/exports/`, is used for one training run, and is
    deleted afterwards (never committed, never in a report; rules 10 and 11);
  - it is never sent anywhere else.
- `recs-train retrain` is still a stub ("not implemented yet" in image `6c79854`, and
  `deploy/nightly.sh` swallows the failure with `|| true`). The desktop-side loop is new work in
  any case: track it with the automated-deploy plan's step 4.

## 5. Re-validate on the Pi

Each of these runs on the Pi with the production compose file and a copy of the production data:

- [ ] **Phase 2 load gate:** `/api/recommend` (KNN, cache miss) through Caddy, p99 < 75 ms at 20
  concurrent clients (`tests/load/run-load.sh <sha> ml-32m`). The gate is defined "on the host
  machine", so it must pass again here.
- [ ] Per-model throughput and latency at 20 concurrent clients, with the same method as §1.
- [ ] Login latency (Argon2id) and peak memory with 4 concurrent logins.
- [ ] Peak memory of the whole stack under load stays under ~2.5 GB.
- [ ] `deploy/restore-drill.sh` and `deploy/smoke.sh` pass.
- [ ] Temperature under the load test stays below the throttling point
  (`vcgencmd measure_temp`, `vcgencmd get_throttled`).

## 6. Cutover

After §2–§5 pass.

1. Take the site offline on the desktop (`deploy/runbooks/take-offline.md`).
2. Final backup with the backup job (`VACUUM INTO` + `integrity_check`). Copy `app.db`, the
   current artifact, `reports/` and `deploy/.env` to the Pi over Tailscale only (`.env` holds the
   TMDB key). The poster cache is optional and refills itself.
3. Restore on the Pi (`deploy/runbooks/restore.md`), `deploy/release.sh <sha>`,
   `deploy/smoke.sh`.
4. Move the public hostname:
   - If Funnel is live: run `tailscale funnel` off on the desktop, rename the desktop node, rename
     the Pi to `intermission`, then follow `deploy/runbooks/funnel-setup.md` on the Pi. The URL and
     `RECS_SITE_ORIGIN` are unchanged, so sessions and the CSRF origin check keep working.
   - If the site is not public yet: nothing moves. `funnel-setup.md` runs on the Pi when the Wave
     3b audit has passed and the owner signs off (CLAUDE.md rule 12).
5. Verify from a phone on mobile data.
6. Keep the desktop stack stopped but intact for one week as the rollback path, then remove its
   production data directory.

## 7. Docs to update

- `docs/spec.md`, *Hosting and deployment*: the host is a Pi 4 (arm64); training runs on a
  separate machine; the load gate's "host machine" means the Pi.
- `deploy/runbooks/deploy.md`: Pi specifics, the artifact upload path, and the #98 items.
- `deploy/runbooks/funnel-setup.md`: the hostname move (§6 step 4).
- `CLAUDE.md`: the production host.

## 8. Order of work

1. §2.1 compiler flag and §2.3 arm64 test job (orchestrator).
2. §2.2 multi-arch publish (#97), with #98's requirements.
3. §3 Pi setup (owner), in parallel with 1–2.
4. §5 validation on the Pi.
5. §4 artifact upload and export pull.
6. §6 cutover, §7 docs.
