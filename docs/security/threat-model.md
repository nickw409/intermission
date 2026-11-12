# Intermission — threat model (pre-launch audit SA0)

Scope: the site as deployed by `deploy/compose.yaml` on the owner's machine, published through Tailscale Funnel.
Written 2026-11-10 before the Wave 3b audit; the audit passes (SA1–SA6) test against this document.

## Assets
| Asset | Where | Why it matters |
|---|---|---|
| Accounts: usernames, Argon2id hashes | `app.db` (recs-api only), pre-migration and nightly backups | Credential reuse elsewhere; account takeover |
| Session tokens (cookie) | browser; SHA-256 only in `app.db` | Account takeover |
| Ratings (behavioural data) | `app.db`; nightly exports (internal IDs only); artifacts (anonymous rows) | Privacy |
| Invite codes | printed once; SHA-256 in `app.db` | Unauthorized signup |
| TMDB API key | `deploy/.env` → recs-api env | Quota abuse / account ban |
| The host machine | owner's home machine running Docker | Pivot into the owner's network; the deploy user is in the docker group (root-equivalent) |
| Availability | single machine, single process per service | Site down |
| Integrity of published results | `data/reports` | Misleading evaluation charts |

## Entry points
- **Public (via Funnel → Caddy :8080):** every `/api/*` route in `contracts/public.yaml`, `/healthz`, the static SPA.
  Unauthenticated: signup, login, movies search/get/similar, models, reports list/get, posters, healthz.
- **Outbound:** recs-api → `api.themoviedb.org` and `image.tmdb.org` (posters, movie details) only.
- **Internal:** recs-api → recs-model over the Unix socket `model.sock` (group `recs`, 0660).
- **Files:** recs-api writes `data/db/`, `data/exports/`, `data/posters/`; recs-train reads exports + raw data and
  writes `data/artifacts/`, `data/reports/`, `data/reports-private/`; recs-model reads artifacts (verified).
- **Operator:** `deploy/*.sh`, systemd timers, `recs-api admin invite create`, runbooks.
- **Supply chain:** pinned C++ deps (SHA-256), Go modules (go.sum), npm lockfile, digest-pinned images,
  SHA-pinned GitHub Actions, public GitHub repo.

## Trust boundaries
1. Internet → Funnel → Caddy: all input untrusted. Caddy enforces body caps, timeouts, headers.
2. Caddy → recs-api: recs-api trusts nothing from the request; Host must be preserved for CSRF checks.
3. recs-api → recs-model: recs-model re-validates everything (defense in depth) and never sees identities.
4. recs-train → artifacts → recs-model: artifacts are hash-verified before use; CURRENT may not escape the root.
5. recs-api → exports → recs-train: the export format is the only interface; no usernames cross it.
6. recs-api → TMDB: response sizes and hosts constrained; the key never leaves recs-api. Movie details text (overview, names) is untrusted: cleaned server-side and rendered as plain text. Accepted (LOW, 2026-11-12 delta audit): until per-IP limits exist, one crawler can spend the shared poster+details TMDB budget and churn the 64 MiB details cache; empty answers caused by our own limiter are sent no-store so they don't linger in browsers.

## Attacker goals and the controls that address them (verified in 3b)
| Goal | Primary controls | Audit pass |
|---|---|---|
| Take over an account | Argon2id, constant-time compare, session rotation, HttpOnly/Secure/SameSite cookies, CSRF (CrossOriginProtection + Origin + X-Requested-With), rate limits | SA1 |
| Enumerate users / invites | identical errors and timing, invite-before-username order, limiter keyed on the submitted name | SA1 |
| Read or change another user's data | every query keyed on the session user; no client-supplied user IDs | SA1, SA2 |
| Inject SQL / break parsing | prepared statements only; strict JSON; fuzzed decoders (Go, libFuzzer) | SA2 |
| Crash or exhaust recs-api / recs-model | body caps (Caddy 64 KB, api 64 KB, model 1 MiB), httplib ≥ 0.63 + pre-routing guard, bounded caches, global limiters, timeouts | SA2, SA3 |
| SSRF via the poster proxy | fixed hosts, regex-validated poster paths, no cross-host redirects, size caps, no proxy env | SA5 |
| Steal the TMDB key | never logged or returned; URL errors stripped | SA4, SA5 |
| Read private files | reports served only from the listing; reports-private never served; O_NOFOLLOW; artifacts verified | SA2, SA5 |
| Learn who rated what | per-user files private; exports without usernames; logs without rating payloads | SA5 |
| Compromise via dependencies or CI | pinning everywhere, gitleaks, Dependabot, minimal workflow permissions | SA4 |
| Escape a container / pivot to the host | non-root, read_only, cap_drop ALL, no-new-privileges, model has no network, only 127.0.0.1:8080 published | SA3 |
| Deface results | reports written only by recs-train; UI rejects unknown schema majors | SA2 |

## Known accepted risks (the owner confirms these in the audit sign-off)
- Per-account login lockout by a third party (spec design); global caps can be exhausted by one client until per-IP
  limits exist (depends on whether Funnel passes the client IP).
- A ~1 byte/s trickler can hold one recs-model worker (only reachable through recs-api).
- recs-api is reachable from local processes on the Docker bridge, bypassing Caddy (local-only).
- The deploy user is root-equivalent through the docker group.
- Funnel hostnames appear in certificate-transparency logs; expect probing within days.
