# I1 — recs-api foundation: database, migrations, auth, sessions, invites, CSRF, rate limits

**Owns:** `api/internal/{db,auth,invites,ratelimit,httpx}/`, `api/migrations/`, `api/cmd/recs-api/admin*.go`
(main.go is a wiring file: it already calls each package's `Register` — don't edit it; if you
need a new hook in `app.Deps`, report it).
**Contract:** `contracts/public.yaml` (auth + account routes) and `contracts/fixtures/public/`.
**Spec:** "Accounts, auth and persistence" (every rule), Phase 2 gate "Auth tests". This is the most
security-sensitive code in the project: vetted primitives only, never invent crypto.

## Gate tests (write first; Go `httptest`, `-race`)
- Signup check order exactly as frozen in public.yaml (`signup` description): shape → invite
  (without consuming) → username → create user + consume invite in one transaction. A test proves
  an unauthenticated caller without a valid invite can never get `username_taken`, and a 409 does
  not consume the invite. The login limiter is keyed on the submitted, case-folded username
  whether or not the account exists (test: 429 behaves identically for real and unknown names).
- Signup and login errors are indistinguishable: unknown user vs wrong password → same status,
  same body, and timing within a tolerance (both paths run one Argon2id hash; assert the code path,
  e.g. a counting hasher in tests, rather than flaky wall-clock comparisons — plus one coarse
  timing check).
- Invalid, expired and used-up invite codes → the same `invalid_invite` error.
- An invite with max_uses = N can't be redeemed more than N times with 50 concurrent signups.
- Expired and logged-out sessions are rejected; sessions rotate on login (old token invalid);
  cookie has HttpOnly, Secure, SameSite=Lax, Path=/, 30-day expiry; only SHA-256 of the token is stored.
- CSRF: every mutating route rejects a request without `Origin`, with a foreign `Origin`, or
  without `X-Requested-With: recs` (403 `csrf_rejected`), and `http.CrossOriginProtection` wraps them.
- Rate limits trigger: login 5/min per account; global caps; 429 `rate_limited` with Retry-After.
  (The per-session `/recommend` limiter is built here as a reusable middleware; J wires it.)
- Account deletion cascades (sessions, ratings, feedback rows gone).
- A SQL-injection corpus (≥ 30 classic payloads) used as username/password/invite is stored or
  compared as literal text; no SQL is ever string-built (also add a test that greps the package
  sources for `fmt.Sprintf` near SQL keywords — cheap guard).
- Bodies > 64 KB → 413 `body_too_large`; malformed JSON → 400 `invalid_json`; unknown fields → 400.
- Migrations: numbered `NNNN_name.sql` in `embed.FS`, applied in one transaction, version in
  `PRAGMA user_version`; a backup (`VACUUM INTO`) is taken before migrating a non-empty DB; running
  twice is a no-op. Schema exactly as the spec (plus `users.ratings_version INTEGER NOT NULL DEFAULT 0`).
- Pools: writer `SetMaxOpenConns(1)`, separate reader pool, WAL, `foreign_keys=ON`, `busy_timeout=5000`.
- Go native fuzz targets (`FuzzXxx`) for every JSON decoder you add (signup, login); a short
  `-fuzztime=10s` smoke in `make fuzz-smoke`. Long fuzz runs happen later (Wave 3).

## Details from the spec (don't miss)
- Argon2id t=3, m=64 MiB, p=4, 16-byte salt, 32-byte key, PHC string; `subtle.ConstantTimeCompare`;
  semaphore caps concurrent hashes at 4.
- Passwords 10–128 chars; usernames 3–32 `[A-Za-z0-9_.-]`, case-insensitive unique.
- Invite codes: 16 random bytes, base32, grouped (e.g. `ABCD-EFGH-...`); stored as SHA-256;
  `used_count` incremented in the same transaction that creates the user.
- `recs-api admin invite create --uses N --expires 14d --note "name"` prints the code once.
- `signup_mode` from config (`invite` default, `open`): open mode ignores invite_code.
- Logs: method, route, status, latency, user ID, request ID — never passwords, tokens, cookies.
- Endpoints: POST /api/auth/signup, /api/auth/login, /api/auth/logout, GET/DELETE /api/me.
  `GET /api/me` rating_count can read the ratings table directly (J owns ratings writes).
