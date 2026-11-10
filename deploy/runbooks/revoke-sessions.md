# Revoke all sessions

Use after a suspected session-token leak, or when every user must log in again.
Sessions are rows in recs-api's `sessions` table; deleting them logs everyone out. Accounts,
ratings and invites are untouched.

Only recs-api may touch `app.db` (CLAUDE.md, service boundaries), so this is a recs-api command:

```bash
docker compose -f deploy/compose.yaml run --rm --no-deps api admin sessions revoke-all
```

> **Gap (to reconcile):** at the time of writing recs-api's `admin` command only has
> `invite create`. Until `admin sessions revoke-all` exists, the fallback is:
>
> 1. take the site offline (`take-offline.md`) and `docker compose -f deploy/compose.yaml stop api`;
> 2. back up (`deploy/backup.sh`);
> 3. with the owner present, run `DELETE FROM sessions;` against `data/db/app.db` using a
>    host `sqlite3` as root (`sudo sqlite3 data/db/app.db 'DELETE FROM sessions;'`), then
>    `sudo chown 10001:10001 data/db/app.db*`;
> 4. `deploy/release.sh <current TAG>` to start recs-api again and smoke-test it.

Afterwards: confirm your own browser session is gone (you are sent to the login page), and
check the api log for a burst of `unauthorized` responses settling down as users log back in.
