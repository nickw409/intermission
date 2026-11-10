# Open signup (owner decision)

Switching from invite-only to open signup changes the threat model (anyone can create
accounts), so it needs the owner's decision **and** a delta security audit of the signup path
first (master plan §5, "After launch").

Before switching, confirm:

- the per-account and global rate limits are in place, and per-IP limits if Funnel passes the
  client IP (`funnel-setup.md`, "Does Funnel pass the client IP?");
- disk alerts are working (`monitoring.md`): open signup can grow `app.db` quickly.

Switch:

1. Edit `deploy/.env`: `RECS_SIGNUP_MODE=open`.
2. `deploy/release.sh "$(sed -n 's/^TAG=//p' deploy/.env)"` (recreates `api`, smoke-tests it).
3. In a private window, sign up without an invite code; it must succeed.

Back to invite-only: set `RECS_SIGNUP_MODE=invite` and run step 2 again. Existing accounts
are unaffected.
