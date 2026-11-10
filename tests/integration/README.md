# Integration, robustness, degradation and deploy gates (Phase 2)

Every gate is a script that can be rerun at any time. Each one builds or reuses what it needs,
binds only `127.0.0.1`, uses its own scratch directory and compose project, mounts MovieLens
read-only, and removes every container, network, volume and scratch file it created on exit.
None of them touches `data/` of the checkout except reading `data/raw/` (override with `RAW_DIR`)
and the load summaries in `data/load-results/`. None of them runs Tailscale.

**They can't reach a real stack.** docker compose lets shell variables override `--env-file`,
so `stack-lib.sh` unsets every variable compose or the deploy scripts read (`COMPOSE_*`,
`RECS_ARTIFACTS_DIR`, `RECS_ENV_FILE`, ...) and exports the scratch `RECS_DATA_DIR`,
`RECS_HTTP_PORT` and `TAG`. Project names are fixed in each script, never inherited, and the
names of real stacks (`intermission`, `intermission-drill`, `intermission-dev`) are refused.
Credentials and invite codes never appear in a process's argv (stdin and 0600 files).

| Gate | Script | Needs |
|---|---|---|
| N1 contract, both sides | `tests/integration/run-contract.sh` | C++ release build (always brought up to date), Go, Node |
| N2 robustness through Caddy, N3 degradation, compose ports | `tests/integration/run-stack.sh <tag>` | images `intermission-*:<tag>` |
| N3 load (p99 < 75 ms, 20 clients) | `tests/load/run-load.sh <tag> [ml-latest-small\|ml-32m]` | images; a quiet host |
| M2 deploy + rollback, restore drill | `tests/integration/run-deploy-drill.sh <sha-A> <sha-B>` | two commits that contain `deploy/` |
| Compose: no published port except 127.0.0.1:8080 | `deploy/tests/check_compose.py` (also run by `run-stack.sh`) | Docker |

Images come from `deploy/build.sh <sha>` (tag = short SHA). `TMPDIR` decides where scratch
directories go.

## N1: contract, both sides (`run-contract.sh`)

`go test` in this directory (module `tests/integration`, whose path sits under the api module's
so it may import `api/internal/modelclient`; see `go.mod`). `TestMain`:

1. trains every model on `tests/fixtures/tiny` (20 users, 30 movies) with the real `recs-train`,
   using small configs, and points `CURRENT` at it;
2. starts the real `recs-model` on a Unix socket, and the real `recs-api` (built from `api/`)
   on a free `127.0.0.1` port with that socket, a scratch SQLite database, invite signup and the
   fixture reports;
3. runs the tests, then checks **every recorded exchange** against `contracts/model.yaml` or
   `contracts/public.yaml` with `validate-observations.mjs` (ajv from `tools/contracts`):
   documented status, response schema (error bodies against `Error`), declared headers,
   `X-Model-Version` on every recs-model response and `X-Request-ID` on every response, and
   coverage: **every documented status of every operation must have been observed** (no
   waivers; only the phase-6 feedback endpoints are excluded). A request must also route to
   the operation it is labelled with (literal path segments beat parameters), and a response
   whose contract declares no content must have no body. Cookie values are redacted from the
   recorded observations.

Tests:

- `TestModelClientEveryOperation`: recs-api's real HTTP client (`modelclient.NewHTTP`) calls
  every operation and the error paths (`StatusError` codes), checking `X-Model-Version`.
- `TestModelRawContract`: raw HTTP over the socket, every operation with valid and invalid input
  (invalid JSON, unknown fields, out-of-range ratings, k, n, limit, 5,001 ratings, duplicate
  IDs, 201 batch IDs, > 1 MiB body, reserved models), `X-Request-ID` echo and generation.
- `TestModelUnavailable`: a second recs-model without an artifact: every route 503
  `model_unavailable` with `X-Model-Version: none`; the client maps it (and a dead socket) to
  `ErrUnavailable`.
- `TestPublicContract`: every public endpoint through recs-api with the real recs-model behind
  it: auth, CSRF, sessions, ratings, cold start and personalised recommendations for every
  model, compare, reports, posters, rate limits (429 + `Retry-After`), the 5,000-rating cap
  (4,995 ratings seeded straight into the harness's scratch `app.db`, then a real 409
  `too_many_ratings` for a new movie and 200 for re-rating one), account deletion, and
  finally recs-model stopped (503 `model_unavailable` in under 1 s; ratings of movies recs-api
  has seen keep working).

Without `RECS_INTEGRATION=1` every test reports SKIP. A failed run keeps its scratch directory
(the path of recs-model's log is printed); `RECS_INTEGRATION_KEEP=1` keeps it after a pass too.
`RECS_INTEGRATION_OBS_OUT=file` saves the observations.

## N2 and N3 through Caddy (`run-stack.sh <tag>`)

Runs `deploy/tests/check_compose.py`, then starts project `intermission-it` on
`127.0.0.1:18090` with the production compose file and images (`stack-lib.sh`), trains all six
models on ml-latest-small with the train image, mints an invite with
`recs-api admin invite create`, and runs `go test ./stack`:

- `TestRobustnessThroughCaddy`: unknown movie IDs, empty ratings (empty body, `{}`, null,
  `[]`), out-of-range ratings (0, -1, 0.4, 5.5, 6, 4.3, 1e309, "5", true), a 65 KB body on every
  endpoint that takes one, and malformed JSON. Each must be the expected 4xx with a JSON
  `Error` body, Caddy's security headers and no `Server` header; all exchanges are then
  validated against `public.yaml`.
- `TestDegradationThroughCaddy`: with recs-model **stopped** (`docker compose stop model`) and
  then **hung** (`docker compose pause model`): `/healthz` degraded, login, `/api/me`, signup,
  rating, re-rating, listing and deleting ratings, logout all work; `/api/recommend` (default
  model and knn) and `/api/recommend/compare` answer 503 `model_unavailable` in under 1 s (the
  slowest is logged). Recommendations work again after recovery.

`KEEP_STACK=1` leaves the stack up for debugging.

## M2 (`run-deploy-drill.sh <sha-A> <sha-B>`)

Uses the owner's scripts: `deploy/build.sh` for both SHAs, a scratch production (env file
`RECS_ENV_FILE`, lock `RECS_LOCK_FILE`, history `RECS_HISTORY_FILE`, data directory, project
`intermission-m2` via `COMPOSE_PROJECT_NAME`, port 18092), an
artifact trained with A's train image, the smoke account created with an invite and signup
through Caddy (runbooks/deploy.md step 6) with ten ratings and `SMOKE_MODEL=knn`, then
`deploy/release.sh A` (re-check), `release.sh B` over A, `release.sh A` (rollback), each
running `deploy/smoke.sh`; then `deploy/backup.sh` and `deploy/restore-drill.sh`
(project `intermission-m2-drill` via `RECS_DRILL_PROJECT`, port 18093), which restores the
backup and runs the smoke test. The checkout's `deploy/history` and the real drill project are
never touched.

## Data directories without sudo

`deploy/init-data.sh` needs root to give the data directories the container UIDs. The scripts
run it, and the final removal of the scratch data, in a throwaway root container
(`stack_root` in `stack-lib.sh`, using the Ubuntu builder base image already pulled by
`deploy/build.sh`).
