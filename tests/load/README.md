# Load gate: `/api/recommend` p99 (Phase 2)

Spec: "`/api/recommend` (KNN, warm cache miss) through Caddy: p99 under 75 ms at 20 concurrent
clients on the host machine."

```bash
deploy/build.sh <sha>                                   # images, once
tests/load/run-load.sh <sha>                            # ml-latest-small, configs/knn.json
KNN_CONFIG=path/to/knn.ml-32m.tuned.json tests/load/run-load.sh <sha> ml-32m
```

`run-load.sh` starts a scratch compose project (`intermission-load`, `127.0.0.1:18091`, its own
data directory; see `tests/integration/stack-lib.sh`), trains `pop` and `knn` (default `knn`)
on the dataset with the train image, mints an invite and runs the load generator (`main.go`,
standard library only). Everything is torn down afterwards.

What the generator does:

- **Setup (untimed):** signs up `-users` accounts (70; signup is limited to 10 per minute, so
  about 6 minutes) and gives each 20 ratings on popular movies from `/api/onboarding`.
- **Measurement:** 20 closed-loop clients, no think time. Each iteration changes one of the
  user's ratings (`PUT`, untimed), which bumps `ratings_version`, so recs-api's recommendation
  cache **misses on every request**; then it times `GET /api/recommend?model=knn&k=20` until
  the body is read. recs-model and recs-api's movie cache are warm (the first 100 requests are
  discarded). Each user is used at most 30 times (recs-api's per-user limit), so the sample is
  70 × 30 − 100 = 2,000 requests.
- Every response must be 200, `cold_start: false`, model `knn`, at least one item and an
  `X-Model-Version` header; any error fails the run, as does p99 ≥ 75 ms.
- Prints a JSON summary (p50/p90/p95/p99/max/mean, throughput, load average before and after).

Run it on a quiet host: the script prints the load average and warns if fuzzers are running.
