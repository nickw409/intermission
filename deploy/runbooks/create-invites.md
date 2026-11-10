# Create invites

Signup is invite-only at launch (`RECS_SIGNUP_MODE=invite`). An invite code is printed once
and stored only as a hash.

```bash
docker compose -f deploy/compose.yaml run --rm --no-deps api \
  admin invite create --uses 1 --expires 14d --note "alex"
```

`admin` never migrates the database (it runs next to the live server). On a new or older
database it fails with "start the server or run `recs-api migrate` first": start the stack, or
run `docker compose -f deploy/compose.yaml run --rm --no-deps api migrate`, then retry.

- `--uses N`: how many accounts the code can create (redemption is race-safe).
- `--expires`: lifetime, e.g. `48h`, `14d`.
- `--note`: who it is for; stored with the invite, never shown to users. Don't put personal
  data beyond a first name or nickname in it.

Send the code over a private channel. The signup page takes it in the "Invite code" field.
To stop a code being used, let it expire (a revoke command is not available yet).
