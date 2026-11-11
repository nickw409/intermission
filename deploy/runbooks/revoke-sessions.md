# Revoke all sessions

Use after a suspected session-token leak, or when every user must log in again.
Revoking sessions does not lock anyone out: anyone with a password can log straight back in.
If credentials may have leaked too, take the site offline first (`take-offline.md`). If `app.db`
itself leaked, sessions are stored only as hashes, but password hashes and invites need their own
response (offline, then decide on forced password resets).
Sessions are rows in recs-api's `sessions` table; deleting them logs everyone out. Accounts,
ratings and invites are untouched.

Only recs-api may touch `app.db` (CLAUDE.md, service boundaries), so this is a recs-api command.
It runs next to the live server; no downtime is needed:

```bash
docker compose -f deploy/compose.yaml run --rm --no-deps api admin sessions revoke-all
```

It prints `revoked N sessions` (N may be 0) and never prints a token. The running server does
not cache sessions, so every revoked cookie gets `401` from the next request on; no restart is
needed. If it instead fails with "start the server or run `recs-api migrate` first", the
database schema is behind this image: deploy the current release first (`deploy.md`), then
rerun.

Afterwards: confirm your own browser session is gone (you are sent to the login page), and
check the api log for a burst of `unauthorized` responses settling down as users log back in.
