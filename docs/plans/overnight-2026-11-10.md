# Overnight run log — 2026-11-10

The owner approved this run on 2026-11-09 and is asleep. Authorizations granted:
- Push feature branches and merge to `main` when CI is green and review agents pass. Never force-push.
- An independent agent reviews the Wave 0 frozen interfaces instead of the owner. Decisions are logged here.
- Turn on branch protection for `main` once CI exists.

Stop conditions (only the affected task stops; the others keep going):
- A gate threshold looks wrong.
- A change would need a protocol change or cross a service boundary.
- An unexpected failure I can't diagnose with confidence.

## Timeline

- Downloaded ml-latest-small and ml-32m (MD5 verified). ml-32m counts match the spec exactly: 32,000,204 ratings, 87,585 movies, 200,948 users.
- Spine: C++ build (debug+ASan/UBSan, release, clang fuzz presets; all green), frozen headers, contracts (public.yaml, model.yaml, report.schema.json, artifact.md), report fixtures, tiny dataset, CSV edge-case fixtures, configs, CI workflow, gitleaks config, Dependabot, SECURITY.md, pre-commit hook.

- PR #1 (Wave 0 spine) passed all 7 CI jobs and was merged; tagged `spine-v1`. Branch protection on `main`: 7 required checks, no force-pushes, no deletions.
- Wave 1 started: A1 (ingest), B1 (split + metrics), C1 (popularity + evaluator), I1 (Go auth). K1 and L1 (web) start as slots free up (4-agent limit).

- Merged: #4 spine fixups, #5 AUC clarification, #6 B1 (split, metrics, bootstrap), #3 A1 (ingest, cache v2). Each had an independent code review and test validation; every finding was fixed before merge. ml-32m ingest: exact counts, parse ~11–14 s, cache load ~5.5 s.
- I1 (auth) security audit: no critical/high findings; mediums being fixed (global rate-limit buckets charged only after a request parses; backup file modes). Test validation: 16/17 mutations caught (the survivor — `==` vs constant-time compare — gets a source-scan guard).
- K1 review: browser security clean; accessibility blocker (unrated slider missing aria-valuenow) and a login race fixed.
- Wave 2 started early (Pipelined): D (item-KNN).

- Wave 2 complete and merged: D (item-KNN), E (content), F (MF-SGD), G (ALS), H (recs-model), J1/J2 (recs-api product endpoints), K2 (web pages), with M (deploy) merging. Every branch had an independent code review or security audit plus test validation (mutation testing); every finding was fixed before merge. A gitleaks false positive (fake test key) is allowlisted by exact fingerprint.
- Phase 0 signed off (`phase-0-done`, docs/phase-0.md). Phase 1 results are in docs/phase-1.md, awaiting owner decisions (KNN scoring formula; content gate).
- Wave 3a started: long fuzz runs (libFuzzer 1 CPU-hour across 4 harnesses; Go fuzz 15 min per decoder) and the integration agent (contract tests against real services, robustness through Caddy, degradation, p99 load, deploy/rollback and restore drills).

## Decisions made without the owner (please review)

| # | Decision | Why | Easy to change? |
|---|---|---|---|
| 1 | ml-32m has no tag genome, so the content model uses the spec's TF-IDF(tags) + genres fallback. | The spec allows it; Tag Genome 2021 is a separate download. | Yes, add genome later. |
| 2 | Users with < 10 ratings are not evaluated, but all their ratings stay in training. | Spec says "keep users with at least 10 ratings"; keeping them in training means website users with few ratings still shape retrained models. Moot for ml-32m and ml-latest-small (min 20 ratings/user). | Yes, before Phase 0 sign-off. |
| 3 | n_test = clamp(round_half_up(0.2·n), 1, 10); validation uses the same rule (separate val_min/val_max, also 1 and 10) on the remaining training ratings. Equal timestamps are ordered by a seeded hash. | The spec doesn't say how to round or break ties. | Yes, before Phase 0 sign-off (protocol). |
| 4 | Users with no relevant (≥ 4.0) test items are excluded from ranking-metric means; the count is reported. | NDCG/recall are undefined for them; including them as 0 would penalize all models equally but distort CIs. | Yes, before Phase 0 sign-off (protocol). |
| 5 | Catalog coverage@10 is reported without a CI (it is a population aggregate, not a per-user mean). Every other metric has a bootstrap CI. | Bootstrapping coverage over 200k users × 1,000 resamples is expensive and conceptually odd. | Yes. |
| 6 | Per-user results go to `data/reports-private/<run_id>.per_user.json`, a directory recs-api never serves; run IDs can't contain `.`, and recs-api only serves IDs from its own listing. | Reports are public; per-user rows would expose dense indices and website users' results. | — |
| 7 | `Model::fit` takes a `TrainContext` (validation set, seed, deterministic flag) in addition to the spec's `(train, cfg)`. | MF early-stops on validation RMSE; the spec's signature has no way to pass it. | — |
| 8 | Own portable RNG (xoshiro256**) instead of `<random>` distributions. | `<random>` distributions differ across standard libraries, which would break byte-identical reports. | — |
| 9 | ml-32m gates run locally in release builds and their reports are committed to `docs/phase-N.md`; CI runs everything on the tiny and ml-latest-small data. | GitHub runners (4 cores, 16 GB) are too small for ml-32m KNN/ALS training; the data can't be committed. | Owner may prefer a self-hosted runner later. |
| 11 | recs-api keeps its own movie cache in SQLite, filled from recs-model responses, so ratings keep working while recs-model is down. | Spec gate: "with recs-model stopped, login, signup and ratings keep working". recs-api can't read artifact files without crossing a service boundary. | — |
| 12 | recs-api calls a cheap `GET /v1/version` before every recommendation-cache lookup. | Phase 5 gate: the cache must never serve an older model version after a swap; polling leaves a stale window. | — |
| 13 | Signup checks the invite before the username, and doesn't consume the invite on `username_taken`; the login limiter is keyed on the submitted username whether or not it exists. | Prevents unauthenticated username probing (spec: signup/login errors indistinguishable). In `open` signup mode, username_taken is inherently visible. | — |
| 14 | Model names `hybrid` and `mmr` reserved in the contracts now; `GET /api/feedback/stats` added for the phase 6 CTR chart. | Avoids editing frozen contracts in phase 6. | — |
| 15 | KNN/content implement a "batch path" (`score_trained_user`) used for the fold-in-equals-batch gate and fold-in Jaccard. | The spec's Model interface had no batch path to compare against. | — |
| 16 | The split hash covers the train/validation/test assignment only, not rating values (as the frozen header says). | A value-only change can't move a rating between sets; recording a separate dataset checksum would need a report schema bump. | Yes (schema v2). |
| 17 | Leakage guard: the split drops a user's tags on their own test movies from `train` (and on validation movies from `train_minus_val`). | A user's tag on a held-out movie reveals that they watched it; the content model builds TF-IDF from tags. | Yes, but changes content-model numbers. |
| 18 | `roc_auc` ignores positives that are also excluded. | Header was ambiguous; throwing would abort the ALS gate on overlapping sets. | Yes. |
| 19 | Rating task: a non-finite predicted rating (e.g. a cold movie) is replaced by the training global mean and counted, instead of aborting the evaluation. | The spec keeps cold test movies; aborting after an hours-long fit wastes the run. | Yes. |
| 20 | The paired comparison reports `user_rmse` (mean of per-user RMSE differences), not a pooled RMSE difference. | The paired bootstrap works on per-user values; calling it "rmse" would contradict the report's pooled RMSE. | Yes (add a pooled paired bootstrap later). |
| 21 | MF fold-in solves [b_u, p_u] jointly as one (k+1)×(k+1) ridge system instead of the spec's literal "damped-mean b_u, then ridge p_u". | That's the fixed point of repeating the spec's two steps; parity on ml-latest-small trained to convergence: 0.972 joint vs 0.906 one-sweep; synthetic fold-in RMSE 0.13 vs 0.20. | Yes (one flag). |
| 22 | ALS planted-structure gate: the synthetic generator uses softmax temperature T = 0.25. At T = 0.5 even the oracle (true affinities) only reaches AUC 0.876, so the > 0.9 gate was unattainable by any model; at T = 0.25 the oracle reaches 0.964 and ALS 0.94. Threshold unchanged; oracle numbers documented in implicit.cpp. | The spec doesn't fix the synthetic noise level. | Yes. |
| 10 | GitHub Actions pinned to commit SHAs; gitleaks 8.30.1 runs in CI with a checksum check. | Public repo supply-chain hygiene. | — |

- An independent reviewer checked the interfaces against the spec before freezing: 5 blockers, 9 should-fix. All blockers and should-fix items were fixed (decisions 2, 6, 11–15 below came from it; also: rating-baseline and paired-comparison outputs, an explicit byte-identical exclusion list, a frozen export format `contracts/export.md`, input limits on compare/feedback/posters, ROC AUC for the ALS gate, and the spine now implements the sparse helpers and `make_dataset` so Wave 1 tasks don't block each other).

## Open questions for the owner

2. **Item-KNN scoring formula (blocks Phase 1 sign-off).** The spec's score Σ s'(r−μ)/Σ|s'| is a rating-prediction formula; used for top-N ranking it promotes obscure items with a single highly rated neighbour. On ml-latest-small: spec formula NDCG@10 = 0.0041; without the Σ|s'| denominator 0.0560; positive neighbours only, unnormalized 0.0579; rating-count popularity 0.0427 (Cremonesi et al. 2010 describe this effect). I kept the spec formula as the default and added `normalize` / `positive_only` as config hyperparameters, selectable on the validation set only. **Your call:** (a) keep the spec formula as "knn" and report it as failing popularity if it does, or (b) amend the spec so "knn" uses the unnormalized top-N score (tuned on validation), or (c) report both as separate models. I'll run the ml-32m numbers for all variants so you can decide with data.
3. **Content model vs its spec gate (blocks Phase 1 sign-off).** The spec's profile Σ(r − μ_u)·v_j cancels any feature shared by every rated movie, so a user who rated only comedies with mixed scores gets few comedies (0/10 for 13 of 20 seeds). The gate test passes only because its synthetic user rates pure comedies high and comedy hybrids low. Independent validation confirmed the first fixture was reshaped to pass (swapping its ratings → 0/20 seeds). The honest test-first version is restored as an explicitly **open, failing gate** (hidden Catch2 tag, documented), not counted as passed. Options: (a) accept as-is and document the limitation; (b) change the profile centering (e.g. damped/global mean instead of the user mean); (c) restate the gate. Also: content NDCG@10 on ml-latest-small is 0.0025 vs 0.0457 for rating-count — expect it to lose to popularity.
4. **MF fold-in parity on ml-32m (Phase 3 risk).** With early stopping, SGD stops before user vectors converge, so trained vectors still carry init noise while fold-in returns the exact ridge solution: parity on ml-latest-small 0.44 (default config) – 0.79 (tuned), 0.97 when trained to convergence. The ml-32m gate (> 0.9) will likely need hyperparameters that converge before validation RMSE turns up (more epochs, slower decay, larger λ), chosen on validation. Nothing loosened; flagged early.
5. **CSP hardening (spec change).** The spec's CSP is `default-src 'self'; img-src 'self' data:; frame-ancestors 'none'`. The deploy audit recommends adding `base-uri 'none'; form-action 'self'`. Approve to update the spec and Caddyfile.
6. **Alert delivery.** Prometheus alert rules exist, but no notification channel is configured (no Alertmanager receiver). Pick one (email, ntfy, Discord webhook…).
1. **gitleaks locally.** I wasn't allowed to install the gitleaks binary on your machine without asking (it's external code). CI scans every push regardless. To also scan before each commit, install it yourself (e.g. `go install github.com/zricethezav/gitleaks/v8@v8.30.1`); the pre-commit hook (`tools/hooks/pre-commit`, already enabled) uses it automatically.

## Follow-ups (not blocking)
- Deploy integration (from M): DB lives at `data/db/app.db` (directory mount for WAL) instead of the spec's `data/app.db`; recs-model runs with `network_mode: none`, so its `/metrics` needs a relay through recs-api or a dedicated listener; no alert delivery channel yet (owner picks); `recs-api admin sessions revoke-all` doesn't exist (runbook has an SQL fallback); smoke test needs `SMOKE_USERNAME`/`SMOKE_PASSWORD` in deploy/.env (account created once via invite).
- recs-api's HTTP connection pool to recs-model must stay ≤ recs-model `--threads` (default 16): each keep-alive connection holds a server worker.
- `Manifest` has no `trained_at`; `/v1/models` reports `created_at` as trained_at.
- `recs-train eval` run IDs omit the dataset, so the same model on two datasets within one second collides ("report already exists"). Include the dataset in the run ID suffix.
- Web: the account menu's rating count goes stale while rating; needs a small `refreshMe()` in `web/src/auth/session.tsx` that doesn't reset the sign-in time used for session-expiry detection.
- Rate-limit message text says "wait a minute" but Retry-After is often ~2 s; reword once with the login tests.

## Carry-forward for the Wave 3b security audit (from I1)
0. (from H) cpp-httplib upgraded 0.18.3 → 0.63.0 (CVE-2025-46728: unbounded chunked bodies/headers; checksum verified); pre-routing refuses Transfer-Encoding and length-less POSTs. Residual: a ~1 byte/s trickler can hold one recs-model worker (no whole-request deadline in httplib; only the recs group can reach the socket); re-verify the pinned version and the pre-routing guards in 3b. Also re-check slow-loris behaviour through Caddy (Caddy should bound request time before it reaches recs-api).

1. Login lockout: anyone can burn a user's 5/min login budget, and the 60/min global login cap lets one attacker block all logins. Inherent to the spec's design until per-IP limits (pending the Funnel client-IP check).
2. CrossOriginProtection runs with no trusted origins: Caddy must preserve `Host`, and `RECS_SITE_ORIGIN` must equal the public origin exactly. Dev proxy must not use changeOrigin.
3. With a valid invite, free vs taken usernames are distinguishable (accepted by the frozen contract).
4. No cap on sessions per user; no "log out everywhere"; expired sessions are only swept on login/signup.
5. Pre-migration backups (`<db dir>/backups/`, mode 0700) contain password hashes — treat as sensitive in backup runbooks.
6. Raising Argon2 parameters later would make unknown-user vs old-account timing drift until rehash-on-login exists.

## Gate status
