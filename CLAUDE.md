# CLAUDE.md

Intermission is a movie recommender built as a learning project. It has five recommenders written from scratch in C++, a Go product API, and a TypeScript web UI, and it is served from the owner's machine through Caddy and Tailscale Funnel.

## Read first

- `docs/spec.md` is the source of truth for every requirement, threshold and acceptance gate.
- `docs/plans/2026-11-09-parallel-build.md` is the master build plan: waves, workstreams, directory ownership, merge order.
- `docs/plans/wave-N/<task-id>.md` holds the detailed step plan for each task, written just before its wave starts.
- `docs/phase-N.md` is the record written after each phase completes.

## Current status

Planning is complete; no code exists yet. Wave 0 (the spine) is next.

- **Phase mode: Pipelined** (approved by the owner on 2026-11-10; plan §2). Code for later phases may start early behind the frozen contracts. Merges to `main` and phase sign-offs still happen strictly in phase order.
- **The repo is public** (`github.com/nickw409/intermission`).
- **The site does not go public** until the Wave 3b pre-launch security audit passes and the owner signs off (plan §5, Wave 3).

## Non-negotiable rules

1. **Service boundaries.**
   - recs-model (C++) never opens `app.db` and never sees user identity.
   - recs-train only reads and writes files and never calls a running service.
   - Only recs-api (Go) handles users, sessions and the database.
   - Crossing one of these lines needs owner sign-off.
2. **Gates first.**
   - Write each acceptance gate as a failing test before the code that satisfies it.
   - Never loosen a tolerance or threshold to make a test pass. If one looks wrong, stop and report it with evidence.
3. **The evaluation protocol is frozen.** Never change the split, the relevance threshold (≥ 4.0) or the metric definitions to improve a model's numbers. Protocol changes need the owner's sign-off and a report `schema_version` bump.
4. **Determinism.**
   - Seed all randomness and record every seed in the report.
   - Each model has a deterministic mode: static OpenMP schedule, no Hogwild-style updates.
   - Accumulate sums over more than about 10⁴ terms in double precision.
5. **Frozen interfaces.** After Wave 0, only the orchestrator may edit these. If you need a change, stop and ask.
   - `core/include/recs/*.hpp`
   - `contracts/` (OpenAPI files, the report schema, the artifact format, fixtures)
   - the wiring files: top-level `CMakeLists.txt`, `api/cmd/recs-api/main.go`, `web/src/routes.tsx`, `.github/workflows/ci.yml`
6. **Ownership.** A workstream agent writes only inside its row of the ownership table (plan §3).
7. **Security.**
   - Use vetted primitives only, and never invent crypto.
   - Every SQL statement is prepared, with bound parameters.
   - Never log passwords, tokens, cookies or rating payloads.
   - Secrets (the TMDB key) come from environment variables, never from the repo.
8. **Dependencies.** Pin every dependency version: FetchContent tags, `go.sum`, `package-lock.json`. There is no runtime network access except the TMDB poster fetch.
9. **Learning codebase.** Prefer clear code over clever code. Each model's `.cpp` starts with a comment block explaining the math, with references.
10. **Data.** Never commit MovieLens data, `data/`, or anything derived from it. Model factors and datasets are never offered for download (license).
11. **Public repo.**
    - Never commit secrets, `.env` (only `.env.example`), `app.db`, backups, logs, personal data, or the machine or tailnet name.
    - gitleaks runs in CI and as a pre-commit hook.
    - If a secret is ever committed, stop and tell the owner: rotate it first, then rewrite history.
12. **No public exposure without the audit.** Never run or script `tailscale funnel` before the Wave 3b security audit has passed and the owner has signed off.

## Layout

```
core/           librecs (C++20 static lib): data/ sparse/ split/ models/ foldin/ eval/ artifact/
train/          recs-train CLI: ingest, train, eval, retrain
model-service/  recs-model: internal HTTP API on a Unix socket
api/            recs-api (Go): auth, sessions, invites, ratings, model client, posters, reports
web/            Vite + React + TypeScript SPA
contracts/      public.yaml, model.yaml, report.schema.json, artifact.md, fixtures/
deploy/         compose, Caddyfile, Dockerfiles, systemd timers, runbooks/
tests/          C++ unit/property tests, fixtures, integration, load, fuzz
tools/          dataset download script
data/           gitignored: raw/, artifacts/, reports/, exports/, app.db, backups/
```

## Commands

These are the planned commands; they work once Wave 0 lands.

```bash
tools/download_movielens.sh ml-latest-small        # also: ml-32m (large, run in background)
cmake --preset debug && cmake --build --preset debug && ctest --preset debug   # ASan+UBSan
cmake --preset release && cmake --build --preset release
build/release/recs-train ingest --dataset ml-latest-small
build/release/recs-train eval --model pop --config configs/pop.json
make -C api check            # go vet, staticcheck, govulncheck, go test -race
npm --prefix web run check   # typecheck (fails on stale generated API types), test, build
docker compose -f deploy/compose.yaml -f deploy/compose.dev.yaml up
```

## Working conventions

- **Data:** use `ml-latest-small` for development and tests. Run ml-32m jobs with release builds only, one at a time, queued in `docs/plans/jobs.md`.
- **Parallel agents:** each one works in its own git worktree and `build/` dir. Run at most 4 concurrent coding agents on this host (8 cores), with at most 3 of them building C++.
- **Reviews:** a task is done when its gates pass locally and in CI. Each task gets a `code-reviewer` pass and a `test-validator` pass on its gate tests. `security-auditor` reviews the auth, API, model-service and deploy work.
- **Doc naming:** kebab-case under `docs/`; date-prefixed plans (`YYYY-MM-DD-name.md`); numbered ADRs (`docs/adr/0001-name.md`). No spaces or non-ASCII characters in filenames.
- **Commits:** do not mention Claude, AI or any assistant, and add no Co-authored-by trailers.
- **Owner-only actions:** Tailscale Funnel setup, the TMDB API key, the GitHub remote, switching `signup_mode` to `open`. Prepare runbooks for these; don't run them.
