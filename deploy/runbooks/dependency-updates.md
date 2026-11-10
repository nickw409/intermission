# Dependency updates for the deploy images

Everything that goes into an image is pinned, so nothing changes until someone bumps a pin.
These are the bumps and when to do them.

| What | Where | How it is bumped |
| --- | --- | --- |
| Base images (golang, ubuntu, node, caddy, distroless) | `deploy/docker/*.Dockerfile`, `tag@sha256:` | Dependabot `docker` (weekly PRs) |
| Prometheus, Grafana, node-exporter | `deploy/compose.yaml` | Dependabot `docker-compose` (weekly PRs) |
| recs-backup Go modules | `deploy/tools/recs-backup/go.mod` | Dependabot `gomod` (weekly PRs) |
| GitHub Actions | `.github/workflows/*.yml`, commit SHAs | Dependabot `github-actions` |
| Ubuntu packages for the C++ builder (cmake, ninja, g++) | `ARG APT_SNAPSHOT` in `model.Dockerfile` and `train.Dockerfile` | **By hand, monthly** (below) |

## APT_SNAPSHOT (monthly, and on any toolchain security notice)

The C++ builder installs its toolchain from `snapshot.ubuntu.com` at a fixed date, so a rebuild
months later gets byte-for-byte the same packages, but it also never gets fixes. Bump it:

- **on the first working day of each month** (next to the restore drill), and
- **within a week of an Ubuntu security notice** for gcc, binutils, cmake, ninja or glibc.

Only the builder uses these packages; the runtime images are distroless, so this is about
build-tool supply-chain freshness, not about what runs in production.

1. Pick the new date: `YYYYMMDDT000000Z`, a day or two in the past (the snapshot must exist).
2. Change `ARG APT_SNAPSHOT=` in **both** `deploy/docker/model.Dockerfile` and
   `deploy/docker/train.Dockerfile`. The two builder stages must stay byte-identical so the
   second build reuses the first one's cache.
3. If Dependabot has a pending `ubuntu` base-image PR, merge it in the same change.
4. Build and run the gates locally:

   ```bash
   deploy/build.sh --force HEAD
   deploy/tests/check_images.sh "$(git rev-parse --short=7 HEAD)"
   deploy/tests/check_edge.sh "$(git rev-parse --short=7 HEAD)"
   ```

   and check the toolchain versions in the build log (`cmake --version`, `g++ --version`).
   A GCC minor bump can surface new warnings, and warnings are errors: fix them in the
   owning workstream rather than reverting the bump for long.
5. Open a PR; the `release` workflow repeats the build and the gates.

A Dependabot PR that changes a Dockerfile's builder digest gets the same check: build, run
the gates, merge. Never replace a digest pin with a bare tag.
