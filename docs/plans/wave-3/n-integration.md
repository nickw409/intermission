# N — Wave 3a: integration, robustness, degradation, load, deploy drills (phase 2)

**Owns:** `tests/integration/**`, `tests/load/**`, plus fixes needed to make cross-service gates pass — make
those fixes on your branch in the owning package, smallest change possible, and list each one (owner, file,
reason) in your report so the orchestrator can review them as cross-workstream edits.
**Builds on:** everything on main: recs-model, recs-api (auth, ratings, movies, recommend, reports, posters),
web, deploy (images, compose, Caddy, release/rollback, backup, restore drill).
**Spec:** Phase 2 gates in `docs/spec.md`.

## Gates (each an automated, rerunnable script or test, documented in `tests/integration/README.md`)
1. **N1 contract, both sides:** the Go modelclient (real HTTP client) against the real recs-model binary serving a
   tiny artifact, every operation in `contracts/model.yaml` (status codes, error shapes, headers incl.
   X-Model-Version and X-Request-ID echo). And recs-api's public endpoints against `contracts/public.yaml`
   with the real recs-model behind it (not the fake).
2. **N2 robustness end to end through Caddy** (compose stack on 127.0.0.1): unknown movie ID, empty ratings,
   out-of-range rating, a 65 KB body, malformed JSON → each a clean 4xx with the Error shape.
3. **N3 degradation:** with recs-model stopped, login, signup and ratings keep working; recommendation endpoints
   return `model_unavailable` within 1 s.
4. **N3 load:** `/api/recommend` (item-KNN, cache miss each time — vary the user or bump ratings_version)
   through Caddy, p99 < 75 ms at 20 concurrent clients on this host. Use a small Go load generator in
   `tests/load/` (no new heavy tools). Train the artifact on ml-latest-small for the test (state it) and, if time
   allows, on ml-32m with the tuned KNN config (`configs/knn.ml-32m.tuned.json`) — report both.
5. **M2 deploy and rollback:** `deploy/release.sh` SHA B over SHA A and back to A, both passing `deploy/smoke.sh`
   (use two real commits from main; build images with `deploy/build.sh`).
6. **M2 restore drill:** `deploy/restore-drill.sh` from a backup passes the smoke test.
7. `docker compose config` shows no published port except 127.0.0.1:8080 (prod project).

Rules: never expose anything beyond 127.0.0.1; never run tailscale; use scratch data dirs (not `data/` of the main
checkout) for the stack — copy what you need; clean up every container/volume/image you create. If a gate fails
because of a real bug, fix it (smallest change, test first) and report it; never loosen a gate threshold.
