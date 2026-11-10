# Intermission — Movie Recommender Build Spec

Nov 9, 2026 · @Nick

## Overview

Build a movie recommender in C++ that people can use through a web UI: they create an account, rate movies, and get recommendations from several algorithms shown side by side. This is a learning project, so each algorithm is implemented from scratch and measured against baselines. This spec is the handoff to the coding agent.

**Goals**

- Implement five recommenders from first principles: popularity, item-KNN, content-based, matrix factorization (SGD) and implicit ALS.
- Build an offline evaluation harness that ranks them honestly against baselines.
- Serve them through a Go product backend and an internal C++ model service, with user accounts, persistent ratings, and recommendations that update instantly when a user rates something.
- Make it publicly reachable for free, with no inbound firewall ports.

* Publish evaluation results in the web UI as charts, so visitors can see how each model performs.

**Non-goals for v1**

- Deep learning models such as two-tower or SASRec (possible later phase).
- GPU acceleration (optional final phase only).
- Email verification, password reset by email, OAuth, mobile apps.
- Horizontal scaling: one process on one machine is the target.

## Decisions and constraints

| Decision | Choice | Why |
| --- | --- | --- |
| Recommender engine and model service | C++20 | Learning goal: implement the math by hand. Data fits in RAM; CPU with OpenMP is fast enough. |
| Product backend (auth, users, ratings, public API) | Go | Memory-safe for untrusted input, and its standard library covers HTTP, SQL and crypto with few dependencies. Builds to a single static binary, and the owner already knows it. |
| Service boundary | Go calls the C++ model service over HTTP on a Unix socket | Industry pattern: product logic separate from model serving. A model crash can't take login down, and the C++ code never sees raw internet traffic. |
| Edge | Tailscale Funnel, then Caddy as the reverse proxy | Funnel provides free public HTTPS with no inbound port. Caddy handles static files, security headers, compression, size limits and access logs. |
| Frontend language | TypeScript | Unavoidable for a browser UI. |
| Analysis and plots | C++ computes every metric and writes versioned JSON run reports; the TypeScript web UI charts them on an Evaluation page | One language for all computation. Visitors can see how the models compare, which is part of seeing it in action. Trade-off: no notebook for quick one-off exploration, so the report format must be rich enough to answer new questions without code changes. |
| Users | Accounts with persistent ratings; invite-only signup at launch | Explicitly chosen as extra learning: auth, sessions, persistence. |
| Storage | SQLite (WAL mode), owned by the Go backend only | One file, no server to run, enough for a hobby-scale site. Exactly one service writes to it. |
| Packaging and deploy | Docker Compose, with images tagged by git SHA | Build once, deploy that exact build. Rollback means running the previous tag. |
| Hosting | The owner's own machine | No budget for a server; opening an inbound firewall port is not allowed. |
| Dataset | MovieLens (GroupLens) | Standard benchmark with ratings, timestamps, genres, tags, tag genome and TMDB links. |
| GPU | Not used until the optional final phase | Premature for this data size. |

## Primer: how recommenders work

Every model here answers one question: given what a user has rated, how much will they like each movie they haven't seen? The models differ in what evidence they use.

| Model | Evidence used | Core idea | Teaches |
| --- | --- | --- | --- |
| Popularity | Everyone's ratings | Rank by a damped mean, so a movie rated once doesn't top the list | Baselines; why they are hard to beat |
| Content-based | Movie features (genres, tag genome) | User profile = weighted average of liked movies' feature vectors; score by cosine similarity | Cold start for new movies; the over-specialization problem |
| Item-KNN | Who rated what | Movies are similar if the same users liked them; score = similarity-weighted sum over the user's rated movies | Collaborative filtering; sparsity; explainability |
| MF-SGD | Star ratings | Learn a k-dim vector per user and movie; their dot product plus biases predicts the rating | Latent factors, regularization, gradient descent |
| Implicit ALS | Whether a user watched or liked a movie | Same factor model, but every unwatched movie counts as a weak negative; solved in closed form, one user or movie at a time | Implicit feedback; alternating least squares |

In production these are layered: **candidate generation** (cheap, a few hundred items) → **ranking** (a heavier model) → **re-ranking** (diversity, filters, rules). This project builds each model as a full ranker first, then adds a hybrid and a re-ranker in phase 6.

**Fold-in** is what makes the web UI work. Website users are not in the trained model. To recommend for them, keep the trained movie side fixed and compute the user's side from their ratings at request time. For KNN and content-based this is a weighted sum. For MF and ALS it is one k×k regularized least-squares solve, about a millisecond. Full retraining runs offline on a schedule.

## System architecture

The site is three long-running services plus one batch job. Only Caddy is reachable from the internet, and each service owns exactly one thing. **Caddy** owns the edge. **recs-api** (Go) owns users and all product data. **recs-model** (C++) owns trained models and the movie catalog. **recs-train** (C++, batch) produces those models.

&#91;embedded content: system architecture · online and offline paths\]

Requests never trigger training. recs-model never sees raw internet traffic or user identities: it gets a list of ratings and returns scores. recs-train never touches the live database. It reads a file that recs-api exports.

**Components**

- **Caddy** (reverse proxy) listens on `127.0.0.1:8080`, the target of Tailscale Funnel. It serves the built frontend, proxies `/api/*` and `/healthz` to recs-api, and sets security headers, compression, request-size limits, timeouts and JSON access logs.
- **recs-api** (Go) handles auth, sessions, invites, ratings, rate limiting, the poster proxy and the run-report listing, and builds every public response. It is the only process that opens `app.db`, and it calls recs-model for anything model-related.
- **recs-model** (C++, on librecs) is stateless and read-only. It holds the current model artifacts and the movie catalog in memory and answers fold-in, recommend, similar and search requests over an internal HTTP API on a Unix socket. It hot-swaps to a new artifact when the `CURRENT` pointer changes.
- **recs-train** (C++, on librecs) runs as a batch job with `ingest`, `train`, `eval` and `retrain` commands. It writes artifact directories and JSON run reports.
- **librecs** (C++ static library) contains data loading, sparse matrices, splitter, models, fold-in, metrics and artifact I/O. It has no HTTP and no database code.
- **frontend** (TypeScript) is built to static files that Caddy serves.

**The retrain loop.** A nightly timer runs three steps:

1. `recs-api export-ratings` writes website ratings to `data/exports/` as CSV, with internal user IDs only and no usernames.
2. `recs-train retrain` merges that export with MovieLens, trains, evaluates, and writes a new artifact plus a run report. It then atomically renames a new `CURRENT` pointer into place.
3. recs-model sees the change, loads and verifies the new artifact, and swaps it in with an atomic `std::shared_ptr` swap. In-flight requests finish on the old model. If verification fails, it keeps serving the old one.

**Repo layout**

```
recs/
  core/            librecs (C++): data/ sparse/ split/ models/ foldin/ eval/ artifact/
  train/           recs-train CLI (C++)
  model-service/   recs-model (C++): http/ handlers/ swap/
  api/             recs-api (Go module): cmd/ internal/{auth,db,invites,ratings,modelclient,posters,ratelimit,httpx}/ migrations/
  web/             TypeScript frontend (Vite)
  contracts/       OpenAPI files for the public API and the internal model API (one source of truth)
  deploy/          compose.yaml, Caddyfile, Dockerfiles, systemd timers, runbooks/
  tests/           C++ unit/property tests, cross-service integration tests
  tools/           dataset download script
  data/            gitignored: raw MovieLens, artifacts/, reports/, exports/, app.db, backups/
```

**Tech stack**

| Concern | Choice | Notes |
| --- | --- | --- |
| C++ build | CMake 3.25+, C++20, Ninja | Warnings as errors; ASan and UBSan in the debug preset |
| Dense linear algebra | Eigen | k×k Cholesky solves for ALS and fold-in |
| Parallelism | OpenMP | Per-user and per-item loops |
| CSV | A real RFC 4180 parser, such as fast-cpp-csv-parser or a hand-written one with tests | MovieLens titles contain quoted commas |
| C++ CLI and JSON | CLI11, nlohmann/json |  |
| C++ HTTP (internal only) | cpp-httplib on a Unix domain socket | Fallback: `127.0.0.1:8090` if the socket is awkward in dev |
| C++ tests | Catch2 v3 plus a seeded property-test helper |  |
| Go version | Current stable (1.25 or later) | Needed for `http.CrossOriginProtection` |
| Go HTTP | Standard `net/http` with `ServeMux` method and path patterns | No web framework |
| Go SQLite driver | `modernc.org/sqlite` | Pure Go, so no cgo and a static binary |
| Password hashing | `golang.org/x/crypto/argon2` (Argon2id) | Parameters in the accounts section |
| Rate limiting | `golang.org/x/time/rate` |  |
| Logging and metrics | `log/slog` (JSON), `prometheus/client_golang` |  |
| Go quality gates | `go vet`, `staticcheck`, `govulncheck`, `go test -race`, native fuzzing |  |
| Migrations | Numbered SQL files in `embed.FS` and a small runner | Version kept in `PRAGMA user_version` |
| Reverse proxy | Caddy 2 |  |
| Frontend | TypeScript, Vite, React; Observable Plot or Recharts | Types generated from `contracts/public.yaml` |
| Packaging | Docker Compose; multi-stage Dockerfiles; distroless runtime images | Each image tagged with the git SHA |
| C++ dependencies | vcpkg manifest mode or CMake FetchContent, all versions pinned | Go: `go.sum`; web: lockfile |

## Data

Develop on `ml-latest-small` (about 100k ratings, so every test runs in seconds). Validate and deploy on [MovieLens 32M](https://grouplens.org/datasets/movielens/32m/): 32,000,204 ratings and 2,000,072 tag applications across 87,585 movies from 200,948 users, released 05/2024.

**Files used:** `ratings.csv` (userId, movieId, rating 0.5–5.0, timestamp), `movies.csv` (title, pipe-separated genres), `tags.csv` (free-text user tags), `links.csv` (IMDb and TMDB IDs). The tag genome (dense movie × tag relevance) ships with some releases such as ml-25m and as the separate Tag Genome 2021 dataset. If the chosen release lacks it, the content model falls back to TF-IDF over `tags.csv` plus genres.

**Ingest pipeline (`recs-train ingest`)**

1. Download with a script that verifies the published checksum. Never commit raw data.
2. Parse with a quoting-aware CSV parser. Fail loudly on malformed rows, with the row number.
3. Remap MovieLens IDs to dense 0-based `uint32` indices. Store both mappings in the artifact.
4. Build CSR (user → movies) and CSC (movie → users) matrices with ratings and timestamps. Sort rows by index.
5. Write a binary cache (header with magic, version, counts, checksum) so later runs skip CSV parsing.

**Website users.** At retrain time, website users' ratings are appended as extra users with IDs after the MovieLens range. Their accounts and ratings live only in SQLite and are never written into any exported dataset.

**Licensing.** The MovieLens license forbids commercial or revenue-bearing use without permission. Redistributing the data or transformations of it is only allowed under the same license, and you must not state or imply endorsement by the University of Minnesota or GroupLens. A free, non-commercial demo that serves recommendations fits within this. The site footer credits GroupLens and does not offer dataset or model-factor downloads. Read the [ml-32m README](https://files.grouplens.org/datasets/movielens/ml-32m-README.html) before going public.

## Models

All models implement one interface, so the evaluator, the server and the comparison view treat them the same way.

```cpp
struct UserState;   // model-specific: factor vector, profile vector, or rated-item list

class Model {
public:
  virtual ~Model() = default;
  virtual std::string_view name() const = 0;
  virtual void fit(const Dataset& train, const Config& cfg) = 0;
  virtual UserState fold_in(std::span<const Rating> ratings) const = 0;   // pure, thread-safe
  virtual void score(const UserState& u, std::span<float> out) const = 0; // one score per movie
  virtual Explanation explain(const UserState& u, ItemIdx item) const = 0;
  virtual void save(const fs::path& dir) const = 0;
  virtual void load(const fs::path& dir) = 0;
};
// Shared, not per-model: recommend(u, k, exclude) = score, mask excluded, nth_element, sort top k.
```

Training users are scored through `fold_in` on their training ratings too. That way offline evaluation exercises exactly the code path the website uses.

### Popularity

Rank by a damped mean, where C is the global mean, nᵢ the count and m a prior strength (tune m; start at 10):

```latex
\text{score}_i = \frac{n_i \bar r_i + m\,C}{n_i + m}
```

Also report a pure count-based variant. Both are baselines that every other model must beat.

### Content-based

- Movie vector: tag-genome relevance if available, else TF-IDF over user tags plus one-hot genres, L2-normalized.
- User profile: Σ (rᵤⱼ − μᵤ)·vⱼ over rated movies, where μᵤ is the user's mean rating.
- Score: cosine(profile, vᵢ). Explanation: the rated movie whose vector contributes most to that score.

### Item-KNN

- Similarity: adjusted cosine on mean-centered ratings, with shrinkage sᵢⱼ · nᵢⱼ / (nᵢⱼ + λ), where nᵢⱼ is the number of users who rated both movies.
- Keep the top M neighbors per movie. Only movies with at least `min_support` ratings (start 20) get neighbor lists. Choose M and λ on the validation split, per dataset (on ml-32m: M = 50, λ = 30,000).
- Score: Σ sim(i, j)·(rᵤⱼ − μᵤ) over the user's rated movies j in i's neighbor list with sim(i, j) > 0. This is the top-N form (Cremonesi et al. 2010). The rating-prediction form, which divides by Σ |sim(i, j)| and keeps negative neighbors, is kept as a config option (`normalize`, `positive_only`) for comparison only. As a ranker it promotes obscure movies with a single neighbor, and on ml-32m it loses to popularity (NDCG@10 0.0039 against 0.0440). Amended 2026-11-10 by owner decision; see `docs/phase-1.md`.
- Explanation: the rated movies with the largest sim × rating contributions ("because you rated X").

### Matrix factorization (SGD, explicit ratings)

```latex
\hat r_{ui} = \mu + b_u + b_i + p_u^\top q_i \qquad \min \sum_{(u,i)} (r_{ui} - \hat r_{ui})^2 + \lambda\left(b_u^2 + b_i^2 + \lVert p_u\rVert^2 + \lVert q_i\rVert^2\right)
```

Shuffle every epoch with a seeded RNG, decay the learning rate, and early-stop on validation RMSE. Fold-in: estimate bᵤ as the damped mean residual, then solve the ridge system for pᵤ with Q and bᵢ fixed.

### Implicit ALS

An interaction is a rating ≥ 3.5. Confidence is cᵤᵢ = 1 + α for interactions and 1 otherwise (start α = 40, then tune). Alternate closed-form solves for users and movies, using the YYᵀ trick so each solve only touches that user's interactions:

```latex
x_u = \left(Y^\top Y + Y^\top (C^u - I) Y + \lambda I\right)^{-1} Y^\top C^u p_u
```

Fold-in for a website user is exactly this solve with Y fixed. Cholesky factorization of a k×k system runs in double precision; factors are stored as float32.

### Numerics and determinism

- Each model has a deterministic mode: fixed seed, static OpenMP schedule, no Hogwild-style lock-free updates. Tests run in this mode.
- Accumulate dot products and sums in double precision wherever a sum runs over more than about 10⁴ terms.
- Every hyperparameter lives in a versioned config file, and that file is copied into each artifact and run report.

## Evaluation protocol

The main metric is NDCG@10 on a temporal holdout. A model that does not beat popularity on it is reported as a failure, not shipped as an improvement.

**Split**

1. Keep users with at least 10 ratings.
2. For each user, sort ratings by timestamp, then hold out the last 20% (at least 1, at most 10). The second-to-last 20% of the remaining training ratings form a validation set for tuning and early stopping.
3. A test item that never appears in training (a cold movie) stays in the test set and counts as a miss for collaborative models. Also report the cold-item share.
4. The split is seeded and its hash goes into every run report.

**Ranking task (primary).** Relevant items are held-out ratings ≥ 4.0. Rank every movie the user has not rated in training, with training items excluded.

| Metric | Definition | Why |
| --- | --- | --- |
| NDCG@10 | Graded gain (rating − 3.5), log₂ discount, normalized by the ideal ranking | Primary metric |
| Recall@10, @50 | Relevant items found / min(k, number relevant) | Candidate-generation quality |
| Precision@10 | Relevant items in the top 10 / 10 | Easy to read |
| MAP@10 | Mean average precision | Standard companion metric |
| Catalog coverage@10 | Distinct movies recommended across all users / catalog size | Shows a model that just recommends hits |
| Mean popularity rank of recommendations | Average popularity rank of recommended movies | Popularity bias |
| Cold-start slice | NDCG@10 for users with fewer than 20 training ratings | Mirrors new website users |

**Rating task (secondary, MF-SGD and baselines).** RMSE and MAE on held-out ratings, against global-mean, user-bias and item-bias baselines.

**Fold-in parity check.** For a sample of 500 training users, compare recommendations from the trained user vector with those from `fold_in` on the same ratings. Report mean Jaccard@10. ALS must exceed 0.95. If it doesn't, fold-in is buggy.

**Statistical honesty.** Report every metric with a 95% bootstrap confidence interval over users (1,000 resamples, seeded). Call a difference "better" only if the intervals don't overlap, or a paired bootstrap on per-user deltas excludes zero.

**Hyperparameter tuning.** Grid or random search on the validation set only. The test set is evaluated once per final config.

**Run report.** Each `recs-train eval` writes `reports/<run_id>.json`, which the web UI charts.

```json
{
  "schema_version": 1,
  "run_id": "2026-11-09T21-14-03Z_als_k64",
  "git_sha": "…", "dataset": "ml-32m", "split_hash": "…",
  "model": "als", "config": { "k": 64, "lambda": 0.05, "alpha": 40, "iters": 15 },
  "train_seconds": 41.2,
  "metrics": { "ndcg@10": { "mean": 0.0, "ci95": [0.0, 0.0] } },
  "per_k": { "recall": [[1, 0.0], [5, 0.0], [10, 0.0], [20, 0.0], [50, 0.0]] },
  "slices": { "cold_users": { "ndcg@10": { "mean": 0.0, "ci95": [0.0, 0.0] } } },
  "training_curve": [[1, 0.0], [2, 0.0]],
  "foldin_jaccard@10": 0.0
}
```

Zeros are placeholders. The schema is versioned, and the UI must reject unknown major versions instead of guessing.

## Accounts, auth and persistence

Accounts live entirely in recs-api (Go). They use a username and password with server-side sessions in SQLite. There's no email, so there's no email verification or reset. This is the most security-sensitive part of the project. Use vetted primitives and never invent crypto.

**Schema (SQLite, WAL mode, `foreign_keys=ON`)**

```sql
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(username) BETWEEN 3 AND 32),
  pw_hash     TEXT NOT NULL,              -- PHC string: $argon2id$v=19$m=65536,t=3,p=4$<salt>$<hash>
  created_at  INTEGER NOT NULL
);
CREATE TABLE sessions (
  token_hash  BLOB PRIMARY KEY,           -- SHA-256 of the cookie token; raw token never stored
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at  INTEGER NOT NULL,
  expires_at  INTEGER NOT NULL
);
CREATE TABLE ratings (
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  movie_id    INTEGER NOT NULL,           -- MovieLens movieId, validated against the loaded catalog
  rating      REAL NOT NULL CHECK (rating BETWEEN 0.5 AND 5.0 AND rating * 2 = CAST(rating * 2 AS INTEGER)),
  updated_at  INTEGER NOT NULL,
  PRIMARY KEY (user_id, movie_id)
);
CREATE TABLE invites (
  code_hash   BLOB PRIMARY KEY,           -- SHA-256 of the code; raw code shown once at creation
  max_uses    INTEGER NOT NULL DEFAULT 1,
  used_count  INTEGER NOT NULL DEFAULT 0,
  expires_at  INTEGER,                    -- NULL = never
  note        TEXT,                       -- who it was given to
  created_at  INTEGER NOT NULL
);
CREATE TABLE feedback (                   -- phase 6: online signals
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  movie_id INTEGER NOT NULL, model TEXT NOT NULL,
  action TEXT NOT NULL CHECK (action IN ('impression','click','not_interested')),
  ts INTEGER NOT NULL
);
```

Migrations are numbered SQL files applied at startup inside a transaction, with the version kept in `PRAGMA user_version`.

**Rules**

- **Passwords:** 10–128 characters, hashed with Argon2id from `golang.org/x/crypto/argon2` (t=3, m=64 MiB, p=4, 16-byte random salt, 32-byte key, per RFC 9106) and compared with `subtle.ConstantTimeCompare`. A semaphore caps concurrent hashes at 4, so a login flood can't exhaust memory. Login failures return the same error and take the same time whether or not the username exists.
- **Sessions:** 32 random bytes from `crypto/rand`, base64url-encoded, in a cookie with `HttpOnly; Secure; SameSite=Lax; Path=/`. Sessions expire after 30 days, rotate on login, and are deleted on logout.
- **CSRF:** wrap all mutating routes in Go's http.CrossOriginProtection, and additionally require an `Origin` header matching the site's own origin and the custom header `X-Requested-With: recs`. Reject otherwise.
- **Rate limits:** a token bucket per account for login (5 per minute) and per session for `/recommend` (30 per minute), plus a global cap. Check during deployment whether Funnel passes the client IP. If it does, add per-IP limits for signup and login. If it doesn't, rely on the global caps.
- **Input limits:** request bodies up to 64 KB, at most 5,000 ratings per user. Every SQL statement is a prepared statement with bound parameters. No string-built SQL anywhere.
- **Account deletion:** `DELETE /api/me` removes the user and cascades to everything they own. Their ratings drop out at the next retrain.
- **Concurrency:** two database/sql pools on the same file, a write pool capped at one connection (SetMaxOpenConns(1)) and a read pool, `busy_timeout` 5,000 ms, writes in short transactions.

**Invite-only signup (launch mode).** The config flag `signup_mode` takes `invite` (default at launch) or `open`. In `invite` mode, signup needs a valid code. The code is checked and `used_count` incremented in the same transaction that creates the user, so one code can't be redeemed twice at once. Invalid, expired and used-up codes all return the same error. Codes are minted with `recs-api admin invite create --uses N --expires 14d --note "name"`, which prints the code once. A code is 16 random bytes in base32, grouped for readability. Switching to `open` is a config change and restart, with no code change.

**Recommendation cache.** An LRU keyed by (user\_id, ratings\_version, model\_version, model\_name) in recs-api holds recommendation responses. A rating change bumps `ratings_version` (an integer column \`users.ratings\_version\`, incremented in the same transaction as the rating write), so the next request recomputes.

## HTTP API

Two APIs, each defined in an OpenAPI file under contracts/. Public endpoints are served by recs-api, all JSON under `/api`. Errors use one shape: `{"error": {"code": "string", "message": "string"}}` with the correct 4xx/5xx status. Movie IDs are MovieLens `movieId`. Dense indices never leave the server.

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| POST | /api/auth/signup | none | `{username, password`, invite\_code} → creates the user and starts a session (invite\_code required while signup\_mode = invite) |
| POST | /api/auth/login | none | Starts a session |
| POST | /api/auth/logout | session | Ends the session |
| GET | /api/me | session | Username, rating count, created date |
| DELETE | /api/me | session | Deletes the account and all its data |
| GET | /api/movies/search?q=&limit= | none | Case- and accent-insensitive title prefix and substring match. Ranks prefix matches first, then by popularity |
| GET | /api/movies/{id} | none | Title, year, genres, rating count, mean rating, TMDB ID |
| GET | /api/movies/{id}/similar?model=&k= | none | Item-to-item neighbors (item-KNN or content model) |
| GET | /api/onboarding?n= | session | Popular, genre-diverse movies the user hasn't rated |
| GET | /api/ratings | session | The user's ratings, newest first |
| PUT | /api/ratings/{movieId} | session | `{rating}` in 0.5 steps; creates or updates |
| DELETE | /api/ratings/{movieId} | session | Removes a rating |
| GET | /api/recommend?model=&k= | session | Top-k with score and explanation for one model |
| GET | /api/recommend/compare?models=pop,knn,als&k= | session | One list per model, for the side-by-side view |
| POST | /api/feedback | session | `{movieId, model, action}` (phase 6) |
| GET | /api/models | none | Loaded models, artifact version, trained-at time |
| GET | /api/reports | none | List of run reports (id, model, dataset, headline metrics) |
| GET | /api/reports/{run\_id} | none | One full run report |
| GET | /api/posters/{movieId} | none | Poster image proxied from TMDB, cached on disk |
| GET | /healthz | none | Liveness and model-loaded check |

**Recommendation response**

```json
{
  "model": "als", "model_version": "2026-11-09T21-14-03Z",
  "items": [
    { "movieId": 4993, "title": "…", "year": 2001, "score": 0.83,
      "because": [ { "movieId": 260, "title": "…", "weight": 0.41 } ] }
  ],
  "cold_start": false
}
```

With fewer than 5 ratings, `cold_start` is true and the server returns popularity results plus a prompt to rate more.

**Posters.** `links.csv` maps movieId to a TMDB ID. The server fetches poster paths from the TMDB API with a key kept in an environment variable, never sent to the browser. Images are cached on disk with a size cap and LRU eviction. On failure the endpoint returns a generated placeholder, never a 5xx. The UI shows TMDB's required attribution. Check TMDB's current API terms before going public.

### Internal model API (recs-model)

recs-model speaks HTTP/1.1 JSON on the Unix socket `/run/recs/model.sock`, which only the `recs` group can access. It is stateless: every call carries the ratings it needs, and it never sees usernames, sessions or cookies. recs-api fills in titles and posters before anything reaches the browser.

| Method | Path | Body or query | Returns |
| --- | --- | --- | --- |
| POST | /v1/recommend | `{model, ratings: [{movie_id, rating}], k, exclude: [movie_id]}` | `{items: [{movie_id, score, because: [{movie_id, weight}]}], ignored}` |
| POST | /v1/compare | `{models: [...], ratings, k, exclude}` | One item list per model |
| GET | /v1/similar/{movie\_id} | `?model=&k=` | Neighbor list with similarity |
| POST | /v1/onboarding | `{n, exclude}` | Popular, genre-diverse movie IDs |
| GET | /v1/movies/search | `?q=&limit=` | Movie IDs ranked by match then popularity |
| POST | /v1/movies/batch | `{ids}` | Title, year, genres, TMDB ID, rating count and mean, for up to 200 IDs |
| GET | /v1/models |  | Loaded models, artifact version, trained-at time |
| GET | /healthz |  | 200 only when a verified artifact is loaded |
| GET | /metrics |  | Prometheus metrics |

**Rules for both sides**

- Every response carries `X-Model-Version`, which recs-api includes in its cache key.
- `X-Request-ID` is created by recs-api and passed through, so one request can be traced across both services' logs.
- recs-model validates every input again even though recs-api already has: at most 5,000 ratings, k ≤ 100, ratings in 0.5 steps. Unknown movie IDs are skipped and counted in `ignored`. Rating payloads are never logged.
- recs-api calls recs-model with a 500 ms deadline through `context`. Only idempotent reads get a single retry, and only on a connection error.
- **Graceful degradation:** if recs-model is down or times out, model endpoints return `503 {"error": {"code": "model_unavailable"}}` and the UI shows a banner. Login, signup, ratings and account pages keep working.
- Contract tests run in CI against `contracts/model.yaml` on both sides: Go client fixtures against the C++ server, and the C++ server against the schema.

## Frontend

The frontend is a TypeScript single-page app (Vite + React), built to static files that Caddy serves from the same origin as the API. It has no server-side rendering and no separate Node server in production.

**Pages**

1. **Sign up / log in.**
2. **Onboarding:** a grid of about 30 movies from `/api/onboarding` with 0.5-step star ratings and a progress bar to 10 ratings. Searching for any movie to rate is also available here.
3. **For you:** the user's top 20 from the default model (ALS once it exists). Poster cards show the explanation ("Because you rated X, Y"), a rate control and "not interested".
4. **Compare models:** three to five columns side by side, one per model, for the same user. Movies that appear in several columns are highlighted. This page is the core of "see it in action".
5. **Movie page:** details, the user's rating control, and a similar-movies row with a model toggle.
6. **My ratings:** sortable list with edit and delete.
7. **Evaluation:** charts from `/api/reports`.
8. **How it works:** a short explanation of each model, linking to its Evaluation results.

**Evaluation page charts** (Observable Plot or Recharts, reading run-report JSON):

- NDCG@10 by model, point plus 95% CI whisker, popularity baseline marked as a reference line.
- Recall@k curves (k = 1–50), one line per model.
- Coverage against NDCG@10 scatter: the accuracy–diversity trade-off.
- Training curves: loss or validation metric by epoch or iteration.
- Cold-user slice against all users, per model.
- A run picker to compare any two runs of the same model, such as k = 32 against k = 128.

**Rules**

- API types are generated from or checked against one shared schema file. Mismatched types are a build error.
- Every `fetch` sends `credentials: 'same-origin'` and `X-Requested-With: recs`.
- Ratings update optimistically and roll back on error. Recommendation lists refetch after a rating change, debounced by 500 ms.
- Accessibility: keyboard-operable star ratings, alt text on posters, and charts with a data table fallback.
- No third-party scripts, analytics or CDNs at runtime. Everything is served from the same origin.

## Hosting and deployment

The site runs on the owner's own machine and is published with Tailscale Funnel. It costs nothing and opens no inbound port: the machine connects out to Tailscale, and Tailscale's relays accept public HTTPS on its behalf and forward it through the tunnel ([guide](https://homelab-guide.pages.dev/homelab-tailscale-funnel)).

| Option | Cost | Public URL | Fit |
| --- | --- | --- | --- |
| **Tailscale Funnel (chosen)** | Free; Funnel is available on the free Personal plan ([comparison](https://dev.to/instatunnel/ngrok-vs-tailscale-funnel-the-ultimate-developers-guide-to-ingress-tunneling-3mh8)) | Stable `https://<machine>.<tailnet>.ts.net` | Automatic TLS, outbound-only. Public ports limited to 443, 8443 and 10000 |
| Cloudflare named tunnel | Free tunnel, but needs your own domain with Cloudflare DNS ([guide](https://flaviocopes.com/cloudflare-tunnel/)) | Your domain | Upgrade path if a domain is ever bought: adds WAF and DDoS protection |
| Cloudflare quick tunnel | Free, no account | Random `trycloudflare.com` URL that changes on restart | Testing only: 200 in-flight request cap, no uptime guarantee ([docs](https://developers.cloudflare.com/tunnel/get-started/index.md)) |

**Setup**

1. Install Docker Engine and Tailscale on the host. Tailscale runs on the host, not in a container.
2. `deploy/compose.yaml` defines `caddy`, `api` and `model`, plus a `train` service under a `jobs` profile. Only Caddy publishes a port, and only on loopback: `127.0.0.1:8080:8080`. `api` and `model` share the socket directory through a named volume and have no published ports.
3. Every container runs as a non-root user with `read_only: true`, `cap_drop: [ALL]` and `security_opt: [no-new-privileges:true]`. Each writable path is mounted explicitly: `api` writes `data/app.db*` and `data/exports/`; `model` reads `data/artifacts/`; `train` writes `data/artifacts/` and `data/reports/`. Memory limits are set per service.
4. Rename the machine to `intermission`, enable MagicDNS and HTTPS, grant the `funnel` node attribute in the tailnet policy ([Tailscale docs](https://tailscale.com/kb/1223/tailscale-funnel)), then run `sudo tailscale funnel --bg 8080`.
5. Host systemd timers handle the batch work: nightly `docker compose run --rm api export-ratings`, then `docker compose run --rm train retrain` at low priority, then the backup job.
6. The Caddyfile sets these headers: `Strict-Transport-Security`, `Content-Security-Policy: default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; form-action 'self'; base-uri 'none'` (`form-action` and `base-uri` don't fall back to `default-src`, so they are set explicitly; added 2026-11-10, security audit SA6-1), `X-Content-Type-Options: nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`. It removes the `Server` header, caps `/api/*` bodies at 64 KB, sets read and write timeouts, and logs to stdout as JSON.

**Security posture.** Funnel hostnames get public TLS certificates, and those appear in certificate-transparency logs. Scanners find exposed `ts.net` services this way ([write-up](https://www.attackresearch.com/post/private-until-it-isn-t-frictionless-exposure-via-tailscale-funnels)). Assume the site will be probed within days of going live. Required before the first public run:

- Every rule in the accounts section implemented and tested.
- Requests fuzzed for at least 1 CPU-hour with no crash under ASan and UBSan (see the phased plan).
- A build that strips debug endpoints. `/healthz` reveals nothing beyond up or down.
- Logs record method, path, status, latency and user ID, never passwords, tokens or cookies.
- Only `/` (and the Funnel port) is exposed. The tailnet has no other services shared through Funnel.

**Limits to accept.** The site is down whenever the machine is off or asleep. Funnel's bandwidth cap isn't published, so posters must be cached and served as small, resized images (for example TMDB's w342 size). Training runs at low priority so the site stays responsive.

## Operations: deploy, observe, recover

Every production change is a versioned image deployed by one command, and every failure is something you hear about from an alert, not from a visitor.

**Build and deploy**

1. CI (GitHub Actions) runs every test and quality gate on each push. On `main` it builds `recs-api`, `recs-model`, `recs-train` and the web bundle into images tagged with the short git SHA.
2. Deploy: `deploy/release.sh <sha>` sets `TAG=<sha>` in `deploy/.env`, runs `docker compose pull` (or loads locally built images), then `docker compose up -d`. A smoke test then checks `/healthz`, logs in as a test account, and fetches recommendations.
3. Rollback: `deploy/release.sh <previous sha>`. The last 5 deployed SHAs are listed in `deploy/history`.
4. Database migrations only ever move forward and must work with both the old and new `recs-api`. That means adding columns, never renaming or dropping them in the same release. A migration runs automatically at startup and takes a backup first.
5. Dev runs the same compose file with `compose.dev.yaml` overrides: plain HTTP, source mounts, debug logging. Production runs the same images that CI tested.

**Observability**

- Logs: JSON lines from all three services, each with `request_id`, method, route, status, latency and `user_id` where relevant. Never passwords, tokens, cookies or rating payloads. Docker's log rotation caps them at 50 MB per service.
- Metrics: `recs-api` and `recs-model` expose `/metrics` on internal ports only. Both track request rate, error rate and latency histograms per route. recs-model also reports model version, fold-in latency and artifact load time. recs-api also reports login failures, invite redemptions and model-client errors.
- Dashboards: an optional `monitoring` compose profile runs Prometheus and Grafana, bound to loopback and reachable only over the tailnet.
- Uptime: a free external uptime monitor checks the public `/healthz` every 5 minutes.
- Alerts: site down for 5 minutes; 5xx rate above 1% over 10 minutes; `/api/recommend` p99 above 250 ms over 10 minutes; nightly retrain, export or backup failed; disk above 85%.

**Backups and recovery**

- A nightly backup of `app.db` uses SQLite's online backup API (`VACUUM INTO`), runs an integrity check, and keeps 7 daily and 4 weekly copies. Model artifacts are not backed up, because retraining rebuilds them.
- A monthly restore drill restores the latest backup into a scratch compose project and runs the smoke test against it. A backup that hasn't been restored doesn't count.
- `deploy/runbooks/` contains one page each for deploy, rollback, restore, rotate the TMDB key, revoke all sessions, create invites, open signup, and take the site offline (`tailscale funnel --bg off`).

## Phased plan and acceptance gates

There are eight phases, and a phase is done only when every gate passes in CI. The site goes live at the end of phase 2, and later models plug in behind the same API.

### Phase 0: Foundation and evaluation harness

Deliverables: CMake project with presets (debug+sanitizers, release), CI, ingest, sparse matrices, splitter, metrics, popularity model, `recs-train eval`, JSON run reports.

- [ ] The CSV parser passes fixtures with quoted commas, escaped quotes, a UTF-8 BOM, CRLF line endings and a malformed row (which must fail with its row number).
- [ ] Ingesting ml-32m yields exactly 32,000,204 ratings, 87,585 movies and 200,948 users.
- [ ] CSR → CSC → CSR round-trip is identical. Property test on random sparse matrices.
- [ ] Split invariant: for every user, every test timestamp is ≥ every train timestamp, with no (user, movie) pair in both sets.
- [ ] Each metric matches hand-computed values on at least 5 fixtures, including no relevant items, all relevant, ties and k greater than the candidate count.
- [ ] Two runs with the same seed produce byte-identical reports, apart from `run_id` and timing fields.
- [ ] Popularity NDCG@10 with a 95% CI is recorded for ml-latest-small and ml-32m.

### Phase 1: Item-KNN and content-based

- [ ] Similarity is symmetric and within \[−1, 1\]. Shrinkage never increases |sim|.
- [ ] Item-KNN beats popularity on NDCG@10 on ml-32m: the paired-bootstrap delta excludes zero.
- [ ] Content model sanity: a synthetic user who rated only movies of one genre gets at least 7 of 10 recommendations sharing that genre.
- [ ] `fold_in` followed by `score` equals the batch path exactly for KNN and content (same arithmetic).
- [ ] Explanations only cite movies the user actually rated.

### Phase 2: Server, accounts and web UI (site goes live)

Deliverables:

- recs-model with the internal API.
- recs-api: migrations, auth, invites, ratings, the model client, the poster proxy, and every public endpoint except feedback.
- The frontend, including the Compare and Evaluation pages.
- The Caddyfile, compose files, CI, release and rollback scripts, systemd timers, backups and runbooks.
- Funnel setup.

* [ ] Auth tests (Go, `httptest`):
  - Signup and login errors are indistinguishable.
  - Expired and logged-out sessions are rejected.
  - Sessions rotate on login.
  - CSRF is rejected without `Origin` or `X-Requested-With`.
  - Rate limits trigger.
  - Account deletion cascades.
  - A SQL-injection corpus is stored as literal text.
  - An invite code can't be redeemed past `max_uses`, even with 50 concurrent signups racing.
* [ ] Public and internal API contract tests cover every endpoint: status codes, error shapes, auth where listed.
* [ ] Robustness, end to end through Caddy: unknown movie ID, empty ratings, out-of-range rating, a 65 KB body and malformed JSON each return a clean 4xx.
* [ ] Fuzzing with no crashes: Go native fuzzing on every recs-api JSON decoder (`go test -fuzz`, 15 minutes each), and libFuzzer on the recs-model request handlers under ASan and UBSan (1 CPU-hour).
* [ ] `go vet`, `staticcheck` and `govulncheck` are clean, and `go test -race` passes.
* [ ] Degradation: with recs-model stopped, login, signup and ratings keep working, and recommendation endpoints return `model_unavailable` within 1 s.
* [ ] `/api/recommend` (KNN, warm cache miss) through Caddy: p99 under 75 ms at 20 concurrent clients on the host machine.
* [ ] Containers run as non-root with read-only root filesystems. `docker compose config` shows no published port except `127.0.0.1:8080`.
* [ ] Deploy and rollback: deploying SHA B over SHA A and rolling back to A both pass the smoke test.
* [ ] A restore drill from a backup passes the smoke test.
* [ ] The Evaluation page renders every chart from fixture reports and rejects an unknown schema major version.
* [ ] The public Funnel URL works end to end from a phone off the home network, and the external uptime monitor reports it up.

### Phase 3: Matrix factorization (SGD)

- [ ] Analytic gradient matches central finite differences in double precision, with relative error below 1e-5 for every parameter block.
- [ ] Synthetic recovery: on rank-5 ground truth plus N(0, 0.1²) noise, held-out RMSE is below 0.15.
- [ ] On ml-32m, RMSE beats the user+item bias baseline (CI non-overlapping).
- [ ] Fold-in parity: Jaccard@10 above 0.9 against trained user vectors.

### Phase 4: Implicit ALS

- [ ] The objective is non-increasing after every half-step, within a relative tolerance of 1e-9.
- [ ] Each per-user solve matches a direct dense solve of the same system to 1e-6.
- [ ] Synthetic recovery of planted structure: on implicit data generated from rank-5 factors, AUC above 0.9.
- [ ] Fold-in parity: Jaccard@10 above 0.95.
- [ ] Highest NDCG@10 of all models on ml-32m, or a written analysis of why not.
- [ ] Training time and peak memory on ml-32m appear in the run report.

### Phase 5: Retrain loop and hot swap

- [ ] A rating made on the website appears in the next export and artifact. A deleted account's ratings do not. The export contains no usernames.
- [ ] Swapping the artifact in recs-model under a 20-client load test causes zero failed requests, and every response carries exactly one `X-Model-Version`.
- [ ] A corrupted or partially written artifact is rejected (checksum), recs-model keeps serving the old one, and the retrain-failed alert fires.
- [ ] The recs-api cache never serves a response from an older model version after a swap.

### Phase 6: Hybrid, diversity and online feedback

- [ ] MMR re-ranking raises catalog coverage@10 by at least 20% (relative), and NDCG@10 stays within the non-re-ranked CI.
- [ ] A weighted hybrid with weights tuned on the validation set beats its best single component, or the result is documented.
- [ ] Impressions, clicks and "not interested" are logged. Click-through rate per model appears on the Evaluation page.

### Phase 7 (optional): GPU

- [ ] A CUDA ALS solver matches the CPU objective within 1e-4 relative per iteration, and NDCG@10 falls within the CPU CI.
- [ ] The speedup on ml-32m is measured and reported.

## Working rules for the coding agent

1. Work one phase at a time, in order. Don't start a phase until the previous phase's gates pass in CI.
2. Write each gate as an automated test before the code that satisfies it. Never loosen a tolerance or threshold to make a test pass. If one seems wrong, stop and report it with evidence.
3. Never change the evaluation protocol (split, relevance threshold, metrics) to improve a model's numbers. Protocol changes need the owner's sign-off and bump the report schema version.
4. After each phase, write `docs/phase-N.md`: what was built, gate results, the headline metrics table, and anything surprising.
5. All randomness is seeded and every seed is recorded in the report.
6. Pin every dependency version. No network access at runtime except the TMDB poster fetch.
7. Keep secrets (TMDB key) in environment variables loaded by systemd, never in the repo.
8. Prefer clear code over clever code in librecs. This is a learning codebase, so each model's file starts with a comment block explaining the math, with references.

Service boundaries are rules, not suggestions. recs-model never opens `app.db`. recs-train never talks to a running service; it reads files and writes files. Only recs-api handles user identity. Any change that crosses one of these lines needs the owner's sign-off.

## Launch decisions

- **Name:** Intermission (repo `intermission`): the break between movies, when you decide what to watch next. The only conflict found is an old Mac audio app from Rogue Amoeba, outside this space. Rejected: "ReelRank" (an active movie review site), "Double Feature" and "Nightcap" (existing movie apps), "Changeover" (collides with film titles in the catalog), and "Lowrank" and "Projectionist" (owner's preference).

* **Hostname:** set the machine name to `intermission` and pick a Tailscale-generated tailnet name instead of anything derived from your email. The public URL becomes `https://intermission.<tailnet-name>.ts.net`. Set both before the first `tailscale funnel` run: the certificate, and the hostname in it, become public from then on.
* **Signup:** invite-only at launch (`signup_mode = invite`). Switching to open signup is the owner's call, made later with a config change.
