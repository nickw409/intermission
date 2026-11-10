# M — Deploy and operations (phase 2)

**Owns:** `deploy/**`, `.github/workflows/release.yml`, `.dockerignore`.
**Spec:** "Hosting and deployment", "Operations: deploy, observe, recover", Phase 2 gates (containers, deploy/
rollback, restore drill). This is infrastructure for a public site: least privilege everywhere.

## Build
- Multi-stage Dockerfiles: `deploy/docker/{api,model,train,web}.Dockerfile`. Builders pinned by digest
  (`golang:<ver>@sha256:…`, a Debian/Ubuntu C++ builder, `node:24@sha256:…`); runtime images distroless
  (`gcr.io/distroless/static-debian12:nonroot` for Go; `gcr.io/distroless/cc-debian12:nonroot` for C++, with
  libgomp copied in) or Caddy's official image pinned by digest. Non-root UID, no shell in runtime images.
  Images tagged by short git SHA (`intermission-api:<sha>` etc.). Web build output baked into the Caddy image.
- `deploy/compose.yaml`: services `caddy`, `api`, `model`, and `train` under profile `jobs`; optional
  `monitoring` profile (Prometheus + Grafana bound to 127.0.0.1). Only `caddy` publishes `127.0.0.1:8080:8080`.
  Every service: `read_only: true`, `cap_drop: [ALL]`, `security_opt: [no-new-privileges:true]`, non-root
  `user:`, explicit tmpfs where needed, memory limits, `restart: unless-stopped`, log rotation (json-file,
  max-size 50m). Named volume for the model socket dir shared by api+model (group `recs`, socket 0660). Data
  mounts exactly as the spec: api rw `data/app.db*` (dir) and `data/exports/`; model ro `data/artifacts/`;
  train rw `data/artifacts/`, `data/reports/`, `data/reports-private/`, ro `data/raw`, `data/exports/`. api reads
  `data/reports/` ro. TMDB key via `env_file: deploy/.env` (gitignored; commit `deploy/.env.example`).
- `deploy/compose.dev.yaml`: plain HTTP, debug logs, source mounts where useful.
- `deploy/Caddyfile`: listen :8080 (Funnel terminates TLS); serve the SPA (try_files → index.html); proxy
  `/api/*` and `/healthz` to api; headers exactly as the spec (HSTS, CSP `default-src 'self'; img-src 'self'
  data:; frame-ancestors 'none'`, nosniff, Referrer-Policy), remove `Server`; `request_body max_size 64KB` on
  `/api/*`; read/write timeouts; JSON access logs to stdout; gzip/zstd; preserve Host (api's CrossOriginProtection
  depends on it); never proxy `/metrics`.
- `deploy/release.sh <sha>` (TAG in deploy/.env, compose up -d, smoke test, append to deploy/history keeping 5),
  `deploy/smoke.sh` (healthz, login with a test account from env, fetch recommendations), rollback = release.sh
  <previous sha>.
- `deploy/systemd/`: timers + services for nightly export-ratings → retrain (nice/ionice low priority) → backup;
  `deploy/backup.sh` (`sqlite3 .backup`/`VACUUM INTO` via the api container or a one-shot, integrity_check,
  keep 7 daily + 4 weekly, files 0600) and `deploy/restore-drill.sh` (restore latest into a scratch compose
  project, run smoke).
- `deploy/alerts/`: Prometheus alert rules for the spec's alerts; `deploy/runbooks/`: deploy, rollback, restore,
  rotate TMDB key, revoke all sessions, create invites, open signup, take site offline (`tailscale funnel --bg
  off`), and **Funnel setup** (owner-only; includes verifying whether Funnel passes the client IP).
- `.github/workflows/release.yml`: on push to main, build all images with the SHA tag (no push to a registry
  unless configured; save as artifacts or build locally — document), SHA-pinned actions, minimal permissions.

## Gate tests (scripts under `deploy/tests/`, run in CI where possible)
- `docker compose -f deploy/compose.yaml config` → only published port is 127.0.0.1:8080; every service
  read_only, cap_drop ALL, no-new-privileges, non-root user. (A small script that parses `compose config --format json`.)
- Caddyfile validates (`caddy validate` in the caddy image) and a container test with curl asserts the security
  headers, Server header absent, 65 KB body → 413, /metrics not exposed.
- Images build; runtime images run as non-root and have no shell.
- release.sh/rollback and restore-drill are exercised later in Wave 3 (scripts must be idempotent and fail loudly).
