# Intermission

*The break between movies, when you decide what to watch next.*

Intermission is a movie recommender you can use in the browser. You create an account, rate some movies, and see recommendations from five algorithms side by side. Every algorithm is implemented from scratch in C++ and measured honestly against baselines, and the evaluation results are published on the site as charts.

> **Status:** in planning. The design is in [`docs/spec.md`](docs/spec.md) and the build plan is in [`docs/plans/2026-11-09-parallel-build.md`](docs/plans/2026-11-09-parallel-build.md). No code has been written yet.

## The models

| Model | Evidence | Idea |
|---|---|---|
| Popularity | Everyone's ratings | Damped mean rating, so a movie rated once can't top the list (the baseline) |
| Content-based | Genres and tags | Profile of the movies you liked, scored by cosine similarity |
| Item-KNN | Who rated what | Movies are similar if the same people liked them |
| MF-SGD | Star ratings | Learned user and movie vectors, trained by gradient descent |
| Implicit ALS | Watched / liked | A factor model solved in closed form by alternating least squares |

New users get recommendations instantly through **fold-in**: the trained movie side stays fixed, and the user's side is computed from their ratings at request time.

Models are compared by NDCG@10 on a temporal holdout, with 95% bootstrap confidence intervals. A model that doesn't beat popularity is reported as a failure.

## Architecture

```
Internet ─► Tailscale Funnel ─► Caddy (127.0.0.1:8080) ─┬─► static web UI (Vite + React + TS)
                                                         └─► recs-api (Go) ── SQLite (app.db)
                                                                  │  HTTP over Unix socket
                                                                  ▼
                                                             recs-model (C++, read-only, hot-swaps models)
                                                                  ▲  model artifacts
                                         nightly: export-ratings ─► recs-train (C++ batch)
```

- **recs-api (Go)** owns users, sessions, invites, ratings and every public response.
- **recs-model (C++)** holds the trained models and the movie catalog. It never sees user identities.
- **recs-train (C++)** ingests MovieLens, trains, evaluates and writes the model artifacts and run reports.
- **librecs (C++)** is the shared library: data loading, sparse matrices, models, metrics and artifact I/O.

## Tech stack

C++20 (CMake, Eigen, OpenMP, Catch2, cpp-httplib) · Go 1.25+ (`net/http`, `modernc.org/sqlite`, Argon2id) · TypeScript (Vite, React, Observable Plot) · Caddy 2 · Docker Compose · GitHub Actions.

## Getting started

These steps will work once the first build wave lands. Prerequisites: CMake 3.25+, Ninja, a C++20 compiler with OpenMP, Go 1.25+, Node LTS, and Docker.

```bash
tools/download_movielens.sh ml-latest-small
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
build/debug/recs-train ingest --dataset ml-latest-small
build/debug/recs-train eval --dataset ml-latest-small --model pop --config configs/pop.json
docker compose -f deploy/compose.yaml -f deploy/compose.dev.yaml up
```

## Repository layout

| Path | Contents |
|---|---|
| `core/` | librecs |
| `train/` | recs-train CLI |
| `model-service/` | recs-model |
| `api/` | recs-api (Go) |
| `web/` | frontend |
| `contracts/` | OpenAPI files, report schema, fixtures |
| `deploy/` | compose, Caddy, runbooks |
| `tests/` | tests |
| `tools/` | scripts |
| `docs/` | spec, plans, phase reports |

## Data and attribution

This project uses the [MovieLens](https://grouplens.org/datasets/movielens/) datasets from GroupLens Research at the University of Minnesota, under their license. This is a free, non-commercial demo, and it is not endorsed by the University of Minnesota or GroupLens. The datasets are not redistributed here: download them with the script above. Poster images come from [TMDB](https://www.themoviedb.org/); this product uses the TMDB API but is not endorsed or certified by TMDB.

## License

No license has been chosen yet.
