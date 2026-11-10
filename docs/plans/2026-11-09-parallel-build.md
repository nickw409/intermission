# Intermission — Parallel Implementation Plan

> **For agentic workers:** This is the master orchestration plan. Execute it with
> `superpowers:subagent-driven-development`: one fresh subagent per workstream task, each in its
> own git worktree (`isolation: "worktree"`), with a spec review and a code review before merge.
> Before each wave starts, the orchestrator writes a detailed TDD step plan per task
> (`superpowers:writing-plans`) in `docs/plans/wave-N/<task-id>.md`, using the interfaces that
> the Wave 0 spine has frozen. The source of truth for every requirement is
> `docs/spec.md` (called "the spec" below).

**Goal:** Build Intermission (C++ recommenders + Go API + TypeScript UI, deployed via Caddy and
Tailscale Funnel) as fast as possible by running independent workstreams in parallel. Every
acceptance gate in the spec still has to pass.

**Architecture:** A short serial "spine" (Wave 0) freezes every interface that crosses a
workstream boundary: C++ headers, OpenAPI contracts, the report JSON schema, the artifact layout,
fixtures, and the build wiring. After that, workstreams own disjoint directories and build against
contracts and fakes, never against each other's unfinished code. Integration happens at defined
merge points, each guarded by the phase gates.

**Tech stack:** C++20 / CMake / Ninja / Eigen / OpenMP / Catch2 / cpp-httplib / nlohmann_json / CLI11;
Go 1.25+ / net/http / modernc.org/sqlite / x/crypto/argon2; Vite + React + TypeScript + Observable Plot;
Caddy 2; Docker Compose; GitHub Actions.

## Global constraints (apply to every task)

- C++20, CMake 3.25+, Ninja, warnings as errors; `debug` preset = ASan+UBSan, plus a `release` preset.
- All dependency versions pinned (CMake FetchContent with exact tags, `go.sum`, `package-lock.json`).
- Gate tests are written **before** the code that satisfies them. Tolerances and thresholds are never loosened (spec rule 2).
- The evaluation protocol (split, relevance ≥ 4.0, metrics) is frozen in Wave 0 and never changed to improve numbers (spec rule 3).
- All randomness is seeded; seeds are recorded in reports. Each model has a deterministic mode (static OpenMP schedule, no Hogwild-style updates).
- Sums over more than about 10⁴ terms accumulate in double precision.
- Each model's `.cpp` starts with a comment block explaining the math, with references (spec rule 8).
- Service boundaries: recs-model never opens `app.db`; recs-train only reads and writes files; only recs-api sees user identity.
- No secrets in the repo (TMDB key comes from env). No runtime network access except the TMDB poster fetch.
- Commits: no AI attribution and no Co-authored-by trailers (owner's global git rules).
- After each phase completes: `docs/phase-N.md` (what was built, gate results, metrics table, surprises).
- **The repo is public.** Never commit secrets, `.env` files (only `.env.example`), MovieLens data, model artifacts, `app.db`, backups, logs, personal data, or the hostname/tailnet name. gitleaks runs in CI and as a pre-commit hook. Commits use the GitHub no-reply email.
- **Nothing is exposed publicly** (`tailscale funnel`) until the pre-launch security audit (Wave 3, SA) has passed and the owner has signed off.

---

## 1. How parallelism is achieved

1. **Contracts first.** Wave 0 writes `contracts/public.yaml`, `contracts/model.yaml`,
   `contracts/report.schema.json`, the librecs public headers, and the artifact manifest format.
   After Wave 0, only the orchestrator may change these, and every change is announced to all
   active agents.
2. **Directory ownership.** Each workstream owns a disjoint set of paths (table in §3). Shared
   wiring files (top-level `CMakeLists.txt`, `api/cmd/recs-api/main.go`, `web/src/routes.tsx`,
   `.github/workflows/ci.yml`) are pre-created in Wave 0 with a slot for every module. Agents fill
   their own module and never edit the wiring. This prevents merge conflicts.
3. **Fakes at every boundary.**
   - C++ models train on the hand-built `tests/fixtures/tiny/` dataset and a `synthetic::` generator, so they don't need the ingest pipeline.
   - Go recs-api tests against `api/internal/modelclient/fake` (an in-process server that replays `contracts/fixtures/model/*.json`), so it doesn't need recs-model.
   - The web UI runs against MSW mocks generated from `contracts/fixtures/public/*.json`, so it doesn't need recs-api.
   - The Evaluation page renders `contracts/fixtures/reports/*.json`, so it doesn't need real runs.
   - recs-model serves a popularity-only artifact built from `tiny/`, so it doesn't need KNN or ALS.
4. **Contract tests replace waiting.** The same fixture files are asserted on both sides: the
   C++ server must produce them, and the Go client must consume them. So when the real services
   meet in Wave 3, any disagreement already showed up as a red test on one side.
5. **Long-running jobs are scheduled, not awaited.** ml-32m downloads, ml-32m training/eval runs,
   the 15 min × N Go fuzz runs and the 1 CPU-hour libFuzzer run are started as background jobs as
   soon as their code is ready. Agents keep coding while they run.

### Machine budget (WSL2, one host)
- The host has 8 cores and 31 GB RAM. Run at most **4 coding agents** at once, and at most **3**
  of them building C++. Sanitizer builds of C++ and `go test -race` are both heavy on CPU and RAM.
- When a wave has more tasks than slots, start them in critical-path order (§6). For Wave 2:
  D, H, J first, then E, K2, M, F, G as slots free up.
- Run at most **one ml-32m job at a time** (32M ratings plus CSR/CSC plus factors take several GB).
  The orchestrator keeps a queue in `docs/plans/jobs.md`.
- Each C++ worktree uses its own `build/` dir. Set `CMAKE_BUILD_PARALLEL_LEVEL` to cores ÷ active C++ agents.

---

## 2. Phase ordering: **Pipelined** (approved by the owner, 2026-11-10)

Spec rule 1 says *"Don't start a phase until the previous phase's gates pass in CI."* Taken
literally, this keeps nearly all parallelism inside each phase. This plan supports two modes:

| Mode | What runs in parallel | Rough speed |
|---|---|---|
| **Strict** (the spec as written) | Parallelism only inside each phase. Phase 2 server/UI work starts after Phase 1 gates are green. | Baseline |
| **Pipelined** (recommended) | **Code** for later phases starts early behind contracts. **Merges to `main` and phase sign-off** still happen in phase order, each one gated. MF-SGD and ALS math are written alongside Phase 1, and the Go/web stacks are written alongside Phase 0. | About 2–3× shorter wall-clock |

Pipelined mode keeps what rule 1 is protecting: nothing is *declared done* or *built on*
until the earlier gates are green. The model math depends only on the `Dataset`/`Model`
interfaces, and the Go/web code depends only on contracts, so starting them early does not build
on unverified work. **The owner approved Pipelined mode on 2026-11-10.** It amends spec rule 1
as follows: code for a later phase may start early, but merges to `main` and phase sign-offs
still happen strictly in phase order, each one gated. The wave tables mark the tasks that start
early (⏩).

---

## 3. Workstreams and ownership

| ID | Workstream | Owns (exclusive write) | Phase |
|---|---|---|---|
| S | Spine (orchestrator) | top-level CMake, presets, `cmake/`, `contracts/`, `configs/`, `core/include/recs/*.hpp` (interfaces), `core/src/{util,foldin,sparse}/`, `core/src/data/make_dataset.cpp`, `core/src/models/registry.cpp`, `api/cmd/recs-api/main.go`, `api/internal/{app,apperr,reqid,contractsdir}/`, `api/internal/modelclient/{client.go,fake/}`, `api/Makefile`, `web/src/routes.tsx`, `web/src/api/`, `train/main.cpp`, `train/commands.hpp`, `tests/support/`, `tests/fixtures/`, `tests/core/spine/`, `.github/`, `.gitignore`, `.gitleaks.toml`, `tools/` | 0 |
| A | Data & ingest | `core/src/data/{csv,movielens,cache}.cpp`, `train/cmd_ingest.cpp`, `tests/core/data/` | 0 |
| B | Split, metrics, bootstrap, reports | `core/src/split/`, `core/src/eval/`, `tests/core/split/`, `tests/core/eval/` | 0 |
| C | Popularity, recommend(), artifact I/O, evaluator + reports, train/eval CLI | `core/src/models/popularity.cpp`, `core/src/models/recommend.cpp`, `core/src/artifact/`, `core/src/report/`, `train/cmd_train.cpp`, `train/cmd_eval.cpp`, `train/cmd_compare.cpp`, `tests/core/models/popularity/`, `tests/core/recommend/`, `tests/core/artifact/`, `tests/core/report/`, `tests/core/integration_tiny/` | 0 |
| D | Item-KNN | `core/src/models/item_knn.*`, `tests/core/models/item_knn/` | 1 |
| E | Content-based (TF-IDF + genres; genome if present) | `core/src/models/content.*`, `tests/core/models/content/` | 1 |
| F | MF-SGD | `core/src/models/mf_sgd.cpp`, `core/src/synthetic/low_rank.cpp`, `tests/core/models/mf_sgd/` | 3 ⏩ |
| G | Implicit ALS | `core/src/models/als.cpp`, `core/src/synthetic/implicit.cpp`, `tests/core/models/als/` | 4 ⏩ |
| H | recs-model service | `model-service/**`, `tests/model-service/`, `tests/fuzz/model/` | 2 ⏩ |
| I | Go foundation: db, migrations, auth, sessions, invites, CSRF, rate limit, httpx | `api/internal/{db,auth,invites,ratelimit,httpx}/`, `api/migrations/`, `api/cmd/recs-api/admin*.go` | 2 ⏩ |
| J | Go product: ratings, modelclient HTTP client, movie cache, rec cache, posters, reports, movies (incl. `/healthz`, `/api/models`, `/api/onboarding`), export-ratings | `api/internal/{ratings,moviecache,posters,reports,movies,recommend,export}/`, `api/internal/modelclient/http.go` | 2 ⏩ |
| K | Web app: shell, auth, onboarding, For You, Compare, Movie, My ratings, How it works | `web/src/` except `web/src/pages/evaluation/` | 2 ⏩ |
| L | Web Evaluation page | `web/src/pages/evaluation/`, `web/src/lib/reports/` | 2 ⏩ |
| M | Deploy & ops: Dockerfiles, compose, Caddyfile, CI images, release/rollback, timers, backups, runbooks, alerts | `deploy/**`, `.github/workflows/release.yml` | 2 ⏩ |
| N | Integration & hardening: cross-service contract tests, e2e robustness, load test, degradation | `tests/integration/**`, `tests/load/` | 2 |
| P | Retrain loop & hot swap | `train/cmd_retrain.cpp`, `core/src/data/merge.cpp`, `model-service/swap/` (handed over from H), `tests/integration/retrain/` | 5 |
| Q | Hybrid, MMR, feedback | `core/src/models/hybrid.*`, `core/src/rerank/`, `api/internal/feedback/` | 6 |

---

## 4. Wave 0 — Spine (serial, orchestrator plus at most 1 helper)

The spine is the only serial work that blocks everything else. Keep it small: interfaces,
fixtures and wiring only. No algorithms.

**S1. Repo bootstrap**
- `git init`, add `.gitignore` (`data/`, `build*/`, `web/node_modules/`, `web/dist/`, `*.db*`), and the spec's directory layout. Remote on GitHub (needed for CI gates).
- `tools/download_movielens.sh <ml-latest-small|ml-32m>`: downloads into `data/raw/<name>/` and verifies the published MD5/SHA. **Start the ml-32m download in the background immediately.**

**S2. C++ build skeleton**
- `CMakeLists.txt`, `CMakePresets.json` (`debug` with `-fsanitize=address,undefined`, `release` with `-O3 -march=native` off by default for reproducibility, `fuzz` with libFuzzer), `cmake/deps.cmake` (FetchContent with pinned tags: Eigen 3.4.0, Catch2 v3.x, nlohmann_json v3.11.x, CLI11 v2.x, cpp-httplib v0.x — the exact tags are recorded in the file).
- Targets: `recs_core` (static), `recs-train`, `recs-model`, `recs_tests`. Each `core/src/<module>/` has its own `CMakeLists.txt` that globs its sources, so agents never touch the top level.
- `tests/support/property.hpp`: seeded property-test helper (`for_seeds(n, base_seed, fn)`).

**S3. Frozen C++ interfaces** (`core/include/recs/`)
```cpp
// types.hpp
namespace recs {
using UserIdx = std::uint32_t; using ItemIdx = std::uint32_t;
struct Rating { ItemIdx item; float value; std::int64_t ts; };
struct Csr {                       // used for both CSR (rows=users) and CSC (rows=items)
  std::uint32_t n_rows = 0, n_cols = 0;
  std::vector<std::uint64_t> indptr;   // size n_rows+1
  std::vector<std::uint32_t> indices;  // sorted within each row
  std::vector<float> values;
  std::vector<std::int64_t> ts;
  std::span<const std::uint32_t> row_indices(std::uint32_t r) const;
  std::span<const float> row_values(std::uint32_t r) const;
};
struct IdMap { std::vector<std::int64_t> to_external; std::unordered_map<std::int64_t, std::uint32_t> to_dense; };
struct MovieMeta { std::string title; int year; std::vector<std::string> genres; std::int64_t tmdb_id; };
struct TagApplication { UserIdx user; ItemIdx item; std::string tag; std::int64_t ts; };
struct Dataset {
  std::string name;
  Csr by_user, by_item;
  IdMap users, items;
  std::vector<MovieMeta> movies;          // indexed by ItemIdx
  std::vector<TagApplication> tags;
  std::optional<GenomeMatrix> genome;     // dense items × tags, if present
};
using Config = nlohmann::json;
}
// model.hpp
struct UserState {                         // superset; each model uses what it needs
  std::vector<Rating> ratings; double mean = 0; double bias = 0; std::vector<float> vec;
};
struct Because { ItemIdx item; float weight; };
struct Explanation { std::vector<Because> because; };   // only items in UserState::ratings
struct Scored { ItemIdx item; float score; };
class Model { /* exactly the spec's interface */ };
std::unique_ptr<Model> make_model(std::string_view name);   // registry: pop, pop_count, knn, content, mf, als
std::vector<Scored> recommend(const Model&, const UserState&, std::size_t k, std::span<const ItemIdx> exclude);
// split.hpp, metrics.hpp, report.hpp, artifact.hpp, csv.hpp — signatures only, frozen here
```
Each model's `.cpp` is a stub that throws `not_implemented` and self-registers, so the build stays green.

**S4. Contracts and fixtures**
- `contracts/public.yaml` and `contracts/model.yaml` (OpenAPI 3.1), covering every endpoint in the spec, the error shape `{"error":{"code","message"}}`, and the `X-Model-Version`/`X-Request-ID` headers.
- `contracts/report.schema.json` (JSON Schema for run report `schema_version: 1`), extending the spec's example with: `seeds`, `metrics` for all ranking metrics, `rating_metrics` (rmse/mae), `cold_item_share`, `peak_rss_bytes`, `per_user` (optional, for paired bootstrap).
- `contracts/artifact.md`: the layout `artifacts/<version>/{manifest.json (sha256 of every file), config.json, catalog.bin, idmap.bin, <model>/…}` and the `CURRENT` pointer file, replaced atomically with rename.
- `contracts/fixtures/{public,model,reports}/*.json`: one valid example and one or more error examples per endpoint, plus 4 fixture reports (pop, knn, content, als), one of them with `schema_version: 2`.
- `tests/fixtures/tiny/{ratings,movies,tags,links}.csv`: about 20 users and 30 movies, built by hand so that every metric and split result can be computed on paper. `tests/fixtures/csv/` holds the edge-case files: quoted comma, escaped quote, BOM, CRLF, malformed row.

**S5. Go and web skeletons**
- `api/go.mod` (Go 1.25), `api/cmd/recs-api/main.go` wiring `db`, `auth`, `invites`, `ratings`, `movies`, `recommend`, `reports`, `posters` through `pkg.Register(mux, deps)` stubs; `api/internal/apperr` (the error-shape writer); `Makefile` targets `vet staticcheck vulncheck test-race`.
- `web/` from Vite React-TS; `openapi-typescript` generates `web/src/api/schema.d.ts` from `contracts/public.yaml`, and `npm run typecheck` fails when the generated file is stale. `web/src/api/client.ts` always sends `credentials:'same-origin'` and `X-Requested-With: recs`. `web/src/routes.tsx` has a placeholder component for each of the 8 pages. MSW handlers come from `contracts/fixtures/public`.

**S6. CI** — `.github/workflows/ci.yml` with jobs `cpp-debug` (build plus Catch2 under ASan/UBSan), `cpp-release`, `go` (vet, staticcheck, govulncheck, `test -race`), `web` (typecheck, test, build), `contracts` (validate OpenAPI and schema; check that fixtures validate). `secrets` (gitleaks over the full history). All jobs must be green on the empty skeleton.
- Public-repo hardening: `SECURITY.md` (how to report vulnerabilities privately through GitHub security advisories); GitHub secret scanning and push protection on; Dependabot for GitHub Actions, Go modules, npm and Docker; branch protection on `main` (CI must pass; no force-pushes); `.gitleaks.toml`; a pre-commit hook script in `tools/hooks/`.

**Spine exit check:** CI green, every stub compiles, and the headers, contracts and fixtures are reviewed by the orchestrator and committed. Tag `spine-v1`.

---

## 5. Waves

`→` = depends on. Each wave's tasks run in parallel. A wave's merge point integrates them onto `main` in the listed order and runs the full gate suite.

### Wave 1 — Phase 0 core (+ ⏩ Phase 2 foundations)

| Task | Agent work (tests first) | Gates covered |
|---|---|---|
| **A1** CSV + ingest + sparse | RFC 4180 parser (BOM, CRLF, quotes, row-numbered errors); loaders for ratings/movies/tags/links/genome; dense ID remap; CSR↔CSC transpose; binary cache (magic, version, counts, xxhash/sha256 checksum); `recs-train ingest` | CSV fixtures; CSR→CSC→CSR property test; ml-32m counts (queued job) |
| **B1** Split + metrics + stats | Temporal split exactly as the spec describes (≥10 ratings, last 20% clamped to [1,10], validation = second-to-last 20% of train, cold items kept, seeded, `split_hash`); NDCG/Recall/Precision/MAP@k, coverage, mean popularity rank, cold-user slice, RMSE/MAE; bootstrap CI (1,000 resamples, seeded) and paired bootstrap; Jaccard@k | Split invariant property test; ≥5 hand-computed fixtures per metric (no relevant items, all relevant, ties, k > candidate count) |
| **C1** Popularity + recommend + artifact + eval | Damped mean (m=10) and count variants; shared `recommend()` (score, mask, nth_element, sort); artifact save/load with manifest checksums; `recs-train train/eval` writing reports that validate against `report.schema.json`; deterministic output | Same-seed byte-identical reports (apart from `run_id` and timing); report validates against the schema |
| ⏩ **I1** Go foundation | Migrations runner (`embed.FS`, `PRAGMA user_version`, backup before migrating, users table including `ratings_version`); two connection pools (writer capped at 1 connection); Argon2id with a hash semaphore of 4; sessions; invites; signup/login/logout/me/delete; CSRF (CrossOriginProtection + Origin + X-Requested-With); rate limits; 64 KB body cap; `admin invite create` | Every auth test bullet in Phase 2, including the 50-way concurrent invite race and the SQL-injection corpus |
| ⏩ **K1** Web shell + auth + design system | Layout, nav, GroupLens/TMDB attribution footer, sign up/log in, a keyboard-operable 0.5-step `StarRating`, API client and error banner (`model_unavailable`) | Component tests: star rating keyboard support; banner shown on 503 |
| ⏩ **L1** Evaluation page | Schema-version guard; NDCG CI whisker plot with popularity reference line; Recall@k curves; coverage-vs-NDCG scatter; training curves; cold-slice comparison; run picker; a data table fallback for every chart | "Renders every chart from fixtures and rejects an unknown major version" |

**Merge point W1:** A1 → B1 → C1 (C1's real-data path is wired to A1 and B1 here). Run popularity eval on ml-latest-small, then queue ml-32m. Merge I1, K1 and L1 independently (they touch nothing in C++).
**Phase 0 sign-off:** all Phase 0 gates green, then write `docs/phase-0.md`.

### Wave 2 — Phase 1 models (+ ⏩ Phase 2 services, ⏩ Phases 3/4 math)

| Task | Depends on | Agent work | Gates |
|---|---|---|---|
| **D** Item-KNN | spine (W1 for ml-32m eval) | Adjusted cosine with shrinkage, top-M=100, min_support=20, OpenMP over items, fold-in = weighted sum, explanations | Symmetry and bounds within [−1,1]; shrinkage never increases \|sim\|; fold_in+score == batch; explanations cite only rated items; **beats popularity on ml-32m (paired bootstrap)** |
| **E** Content | spine | TF-IDF(tags)+one-hot genres, L2 norm (genome path if present); profile Σ(r−μ)v; cosine; explanation | One-genre synthetic user gets ≥7/10 recommendations in that genre; fold_in parity; explanations cite only rated items |
| ⏩ **F** MF-SGD | spine | Biased MF, seeded shuffle, LR decay, early stopping on validation RMSE, ridge fold-in; deterministic mode | Finite-difference gradient check < 1e-5; rank-5 synthetic RMSE < 0.15 (ml-32m gates wait for W4) |
| ⏩ **G** ALS | spine | Confidence weights, YᵀY trick, double-precision Cholesky via Eigen, float32 storage, fold-in = the same solve; peak RSS recorded | Non-increasing objective per half-step (1e-9); per-user solve matches dense solve to 1e-6; planted-structure AUC > 0.9 |
| ⏩ **H** recs-model | spine (C1 merged for artifact loading) | cpp-httplib on a Unix socket (TCP fallback); every `/v1/*` route; input revalidation (≤5,000 ratings, k ≤ 100, 0.5 steps, `ignored` count); catalog search (case- and accent-insensitive, prefix first, then popularity); genre-diverse onboarding; `/metrics`; logs that never contain rating payloads; `shared_ptr<const Bundle>` holder (swap logic stubbed until P) | Server produces every `contracts/fixtures/model` response; libFuzzer harness builds and is queued for 1 CPU-hour |
| ⏩ **J** Go product | I1 merged | modelclient (500 ms deadline, a single retry only for idempotent reads and only on connection errors, `X-Request-ID`, 503 `model_unavailable`); ratings CRUD that bumps `ratings_version` in the same transaction; LRU cache keyed by (user, ratings_version, model_version, model); cold_start under 5 ratings; movies/search/similar/onboarding/models; reports listing from `data/reports/`; poster proxy (disk LRU, w342, placeholder on failure); `export-ratings` | Contract tests vs the fake model server for every public endpoint; Go fuzz targets for every JSON decoder (queued, 15 min each); `-race` clean |
| ⏩ **K2** Web pages | K1 | Onboarding (grid of 30, progress to 10, search); For You (explanations, rate, not interested); Compare (3–5 columns, overlap highlighting); Movie page with similar-model toggle; My ratings (sortable, edit, delete); How it works; optimistic rating updates with rollback; 500 ms debounced refetch | Component and MSW tests per page; a11y checks (axe) |
| ⏩ **M** Deploy | spine | Multi-stage Dockerfiles (distroless, non-root), compose (read_only, cap_drop ALL, no-new-privileges, memory limits, only `127.0.0.1:8080` published, socket volume), `compose.dev.yaml`, Caddyfile (headers, CSP, 64 KB cap, timeouts, JSON logs, `Server` header removed), `release.sh` with history and smoke test, systemd timers (export → retrain → backup), `VACUUM INTO` backups (7 daily, 4 weekly) with integrity check, the 8 runbooks, alert rules | `docker compose config` port check; non-root and read-only checks |

**Merge point W2:** D and E (Phase 1 gates on ml-32m: queue the KNN run first, it's the critical one) → **Phase 1 sign-off** → H, J, K2, M merged (in Strict mode they only *start* now). F and G stay on their branches, or merge with their eval gates marked pending until W4.

### Wave 3 — Phase 2 integration, security audit, go-live

Wave 3 has three steps in order: **3a** integration and hardening → **3b** security audit → **3c** go-live.

**3a — integration and hardening** (parallel)

| Task | Work |
|---|---|
| **N1** Cross-service contract tests | Go client fixtures against the real C++ server; C++ server checked against `model.yaml` |
| **N2** E2E robustness through Caddy | Unknown movie ID, empty ratings, out-of-range rating, a 65 KB body, malformed JSON → clean 4xx |
| **N3** Degradation and load | recs-model stopped → auth and ratings still work, model routes return 503 within 1 s; KNN `/api/recommend` p99 < 75 ms at 20 concurrent clients (`k6` or `vegeta`) |
| **N4** Fuzz sign-off | Collect the background fuzz results (Go: 15 min per decoder; libFuzzer: 1 CPU-hour under ASan/UBSan) |
| **M2** Deploy drills | Deploy SHA B over A, roll back to A, smoke test both; restore drill from backup |
| **Owner** Funnel | Rename the machine to `intermission`, set the tailnet name, grant the funnel attribute, `tailscale funnel --bg 8080`, check that Funnel passes the client IP (decides whether per-IP limits are added), test from a phone, set up the uptime monitor. **Requires the owner — the agent prepares a runbook and does not run these steps.** |

N1–N4 and M2 run in parallel (4 agents). The Owner Funnel row is done in 3c, not here.

**3b — pre-launch security audit (gate: no public exposure until it passes)**

The audit runs against the exact image SHA that will go live, deployed locally through Caddy on
`127.0.0.1:8080` (staging = production compose without Funnel).

1. **SA0 — threat model** (orchestrator): `docs/security/threat-model.md` covers assets
   (accounts, sessions, ratings, invite codes, the TMDB key, the host machine), entry points
   (every public route, poster fetches, the nightly jobs), trust boundaries (Funnel → Caddy →
   recs-api → Unix socket → recs-model; files between recs-api, recs-train and recs-model), and
   attacker goals.
2. **SA1–SA6 — parallel audit passes** (`security-auditor` agents, independent of the agents
   that wrote the code; give them the code and the threat model, not the authors' conclusions):

   | Pass | Scope |
   |---|---|
   | **SA1** Auth and sessions | Argon2id parameters and the hash semaphore; equal error *and* timing for unknown users; session entropy, rotation, expiry, logout, cookie flags; CSRF (CrossOriginProtection + Origin + X-Requested-With) on every mutating route; the invite race; rate limits and global caps; account deletion cascade; authorization on every route (user A can never read or change user B's data) |
   | **SA2** Input handling and memory safety | Every Go JSON decoder and query parameter (sizes, types, limits); every SQL statement prepared; recs-model request parsing, CSV/artifact loaders and the binary cache reader (bounds, integer overflow, untrusted lengths), plus a review of the fuzz corpus coverage; path traversal in `/api/reports/{run_id}` and the poster cache |
   | **SA3** Edge and containers | Caddyfile headers and CSP actually served (checked with curl, not just config); the 64 KB cap; timeouts; `Server` header removed; no debug endpoints and no `/metrics` reachable through Caddy; `/healthz` reveals only up/down; compose: non-root, `read_only`, `cap_drop: ALL`, `no-new-privileges`, memory limits, only `127.0.0.1:8080` published, socket permissions limited to the `recs` group |
   | **SA4** Secrets and supply chain | gitleaks over the full git history; no secrets in images (`docker history`, image filesystem scan); TMDB key never sent to the browser; `govulncheck`, `npm audit --omit=dev`, Trivy image scan; every dependency pinned; GitHub Actions pinned to commit SHAs |
   | **SA5** Privacy, logging, outbound requests | Logs never contain passwords, tokens, cookies or rating payloads (grep the logs from a full e2e run); `export-ratings` contains no usernames; the poster proxy can only reach TMDB hosts (no SSRF via movie ID or redirects); the TMDB-response-size cap; GroupLens/TMDB attribution and no dataset or model-factor downloads |
   | **SA6** Dynamic scan | OWASP ZAP baseline + API scan (from `contracts/public.yaml`) against staging; a manual check of the spec's "Required before the first public run" list |
3. **Findings** go in `docs/security/audit-2026-MM-DD.md`, with severity (critical/high/medium/low), evidence, and fix status. Each fix goes through the normal task protocol (§7). The auditor that found an issue re-tests its fix.
4. **Gate:**
   - Zero open critical or high findings.
   - Every medium finding is either fixed or explicitly accepted by the owner, with a reason, in the audit doc.
   - Every pre-launch item in the spec's Security posture section is checked off.
   - **The owner signs off in writing in the audit doc.**

**3c — go-live** (owner, following the runbook): the Owner Funnel row above. Then **Phase 2 sign-off → site live**, and write `docs/phase-2.md`.

**After launch:** any later wave that changes recs-api, recs-model's request handling, the Caddyfile, compose or the auth flow gets a **delta audit** (the relevant SA passes, scoped to the diff) before it is deployed. Repeat the full audit if the threat model changes, for example when switching `signup_mode` to `open`.

### Wave 4 — Phases 3 and 4 (MF-SGD and ALS on real data)

F and G run in parallel. In Pipelined mode their math is already done, so this wave is mostly ml-32m runs, tuning on validation only, the fold-in parity checks (MF > 0.9, ALS > 0.95), the RMSE-vs-bias-baseline CI, and the "ALS highest NDCG or written analysis" item. Register both in recs-model and make ALS the default For You model. Sign off phase-3 and phase-4 docs in order.

### Wave 5 — Phase 5 retrain loop and hot swap (2–3 agents)

**P1** `recs-train retrain` (merge export with MovieLens, website users after the ML ID range, atomic `CURRENT` rename) · **P2** recs-model swap watcher (verify checksums, atomic `shared_ptr` swap, keep the old model on failure, retrain-failed alert) · **P3** recs-api cache keyed by model version plus integration tests (rating appears in the next artifact, deleted account's ratings excluded, no usernames in export; zero failed requests under 20-client load during a swap).

### Wave 6 — Phase 6 (3 agents)

**Q1** MMR re-ranker (+20% coverage, NDCG within the CI) · **Q2** weighted hybrid tuned on validation · **Q3** `POST /api/feedback`, impression/click/not-interested logging, CTR chart on the Evaluation page.

### Wave 7 — Phase 7 (optional, 1 agent): CUDA ALS.

---

## 6. Critical path

```
S (spine) ─► A1 ─► C1(real data) ─► ml-32m pop eval ─► D on ml-32m ─► Phase 1 ✔
                                                     │
            I1 ─► J ───────────┐                     ▼
            H ─────────────────┼──► W3a integration + fuzz + drills ─► W3b security audit ─► LIVE
            K1 ─► K2, L1, M ───┘
```
The longest chain is **spine → ingest → KNN beating popularity on ml-32m → integration → live**.
To keep it short: start the ml-32m download in S1, have A1 build the binary cache early so later
runs skip CSV parsing, give D's ml-32m run first priority in the job queue, and launch fuzzing as
soon as H and J have decoders. Fuzzing doesn't need to wait for W3.

## 7. Per-task protocol for subagents

1. Read the spec sections named in the task, this plan's Global constraints, and the frozen headers/contracts. Do not edit files outside the task's ownership row. If an interface needs changing, stop and ask the orchestrator.
2. Write the gate tests first and confirm that they fail.
3. Implement, then run `cmake --build --preset debug && ctest --preset debug` (C++), `make -C api check` (Go), or `npm --prefix web run check` (web).
4. The task is done when its gates pass locally *and* in CI on the branch. Report: files changed, gate results with output, anything surprising, and any long job queued.
5. Review: a `code-reviewer` agent pass, plus a `test-validator` pass on gate tests (to catch tolerances quietly loosened or tests gamed). `security-auditor` reviews I1, J, H and M during development; that does not replace the Wave 3 pre-launch audit.
6. The orchestrator merges in the order listed at the merge point, re-runs CI on `main`, and tags `phase-N-done` at sign-off.

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| A frozen interface turns out to be wrong after Wave 0 | The orchestrator owns all changes; a change is one commit to the headers/contracts, rebased into every active worktree, and active agents are notified |
| ml-32m has no tag genome | The content model uses the TF-IDF fallback (the spec allows it); Tag Genome 2021 is optional for later |
| RAM is exhausted on WSL2 by parallel sanitizer builds plus ml-32m jobs | Concurrency limits from §1; ml-32m jobs run in release builds only, one at a time |
| KNN fails to beat popularity on ml-32m | Report it per the spec, tune M/λ/min_support on validation only, and do not change the protocol |
| An integration surprise at W3 | Contract tests on both sides from W2 onward; the fake model server replays the same fixtures that the C++ server must produce |
| Funnel, the TMDB key and the GitHub remote need the owner | Listed as owner actions; agents prepare runbooks and env templates and never handle secrets |
| A secret or personal data leaks into the public repo | gitleaks in CI and pre-commit, GitHub push protection, a strict `.gitignore`, the no-reply commit email; if a leak happens anyway, rotate the secret first, then rewrite history |
| The site is probed soon after launch (CT logs reveal it) | Nothing goes public before the Wave 3b audit; delta audits after launch; the runbook covers taking the site offline (`tailscale funnel --bg off`) |
