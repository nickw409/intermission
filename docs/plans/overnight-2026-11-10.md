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
| 10 | GitHub Actions pinned to commit SHAs; gitleaks 8.30.1 runs in CI with a checksum check. | Public repo supply-chain hygiene. | — |

- An independent reviewer checked the interfaces against the spec before freezing: 5 blockers, 9 should-fix. All blockers and should-fix items were fixed (decisions 2, 6, 11–15 below came from it; also: rating-baseline and paired-comparison outputs, an explicit byte-identical exclusion list, a frozen export format `contracts/export.md`, input limits on compare/feedback/posters, ROC AUC for the ALS gate, and the spine now implements the sparse helpers and `make_dataset` so Wave 1 tasks don't block each other).

## Open questions for the owner

1. **gitleaks locally.** I wasn't allowed to install the gitleaks binary on your machine without asking (it's external code). CI scans every push regardless. To also scan before each commit, install it yourself (e.g. `go install github.com/zricethezav/gitleaks/v8@v8.30.1`); the pre-commit hook (`tools/hooks/pre-commit`, already enabled) uses it automatically.

## Gate status
