# Rotate the TMDB API key (owner only)

The key lives only in `deploy/.env` (mode 0600, gitignored) and reaches recs-api as the
`TMDB_API_KEY` environment variable. recs-api never sends it to the browser and logs only
whether it is set.

1. Create a new key in the TMDB account settings (keep the old one active for now).
2. Edit `deploy/.env`: `TMDB_API_KEY=<new key>`. Check the file is still 0600:
   `stat -c %a deploy/.env`.
3. Recreate recs-api with the new environment and smoke-test it:
   `deploy/release.sh "$(sed -n 's/^TAG=//p' deploy/.env)"`
   (compose sees the changed variable and recreates only `api`).
4. Open a movie page and confirm a poster that is not yet cached loads (cached posters on disk
   don't prove anything). recs-api returns a placeholder, never an error, if the key is bad,
   so check its log: `docker compose -f deploy/compose.yaml logs --since 5m api | grep -i tmdb`.
5. Revoke the old key at TMDB.
6. Confirm the key is in no log or image:
   `docker compose -f deploy/compose.yaml logs api caddy | grep -c "<first 8 chars of key>"`
   must print 0.

If the key leaked (committed, pasted, logged): revoke it at TMDB **first**, then do the steps
above. If it was committed to git, also follow the secret-leak procedure in `SECURITY.md`
(rotate, then rewrite history).
