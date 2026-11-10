# Load gate: `/api/recommend` p99 (Phase 2)

Spec: "`/api/recommend` (KNN, warm cache miss) through Caddy: p99 under 75 ms at 20 concurrent
clients on the host machine."

**The sign-off run is ml-32m with the tuned KNN config** (`configs/knn.ml-32m.tuned.json`,
picked automatically for ml-32m). ml-latest-small is a quick check.

```bash
deploy/build.sh <sha>                                   # images, once
WAIT_QUIET=1 tests/load/run-load.sh <sha> ml-32m        # sign-off
tests/load/run-load.sh <sha>                            # quick check: ml-latest-small
```

`run-load.sh` starts a scratch compose project (`intermission-load`, `127.0.0.1:18091`, its own
data directory and a scrubbed environment; see `tests/integration/stack-lib.sh`), trains `pop`
and `knn` (default `knn`) on the dataset with the train image, mints an invite and runs the load
generator (`main.go`, standard library only). Everything is torn down afterwards. The JSON
summary is written to `data/load-results/<UTC time>-<dataset>.json` (gitignored; `LOAD_OUT`
overrides) and printed.

What the generator does:

- **Setup (untimed):** signs up `-users` accounts (70; signup is limited to 10 per minute, so
  about 6 minutes) and gives each 20 ratings on popular movies from `/api/onboarding`.
- **Measurement, in rounds:** each of the 20 clients takes a different user and changes one of
  that user's ratings (`PUT`, untimed), which bumps `ratings_version`, so recs-api's
  recommendation cache **misses on every request**. When all 20 PUTs are done, the 20
  `GET /api/recommend?model=knn&k=20` requests are released together and timed until the body
  is read, so 20 cache-missing GETs are really in flight at once. The generator reports the
  peak and the time-weighted average of GETs in flight, and **fails if the peak is below 20**.
- recs-model and recs-api's movie cache are warm: the first 100 requests (5 rounds) are
  discarded. Each user is used at most 30 times (recs-api's per-user limit): 70 × 30 / 20 = 105
  rounds, 2,000 timed requests.
- Every response must be 200, `cold_start: false`, model `knn`, at least one item and an
  `X-Model-Version` header; any error fails the run, as does p99 ≥ 75 ms.
- The summary has p50/p90/p95/p99/max/mean, in-flight GETs, and the load average before and
  after.

Run it on a quiet host: the script prints the load average and warns if fuzzers are running;
with `WAIT_QUIET=1` it does the setup, then waits until no fuzzer is running before measuring.
The invite code reaches the generator through `LOAD_INVITE`, not its command line.
