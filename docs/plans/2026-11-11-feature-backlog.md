# Feature backlog (owner requests, 2026-11-11)

Ideas the owner wants after the For you controls (model selector and genre chips) ship. Not
scheduled yet. Each needs a spec amendment and, where noted, a contract change, a delta security
audit and phase-order sign-off before it merges. Notes below are first thoughts, not designs.

## 1. Infinite scrolling on For you

- Today `k` is capped at 100 (contracts) and each request recomputes the ranking. Scrolling needs
  paging: an `offset` (or opaque cursor) on `/api/recommend`, with the artifact version in the
  cursor so a hot swap mid-scroll restarts cleanly instead of mixing two models' orders.
- recs-model already ranks the whole catalog per request; paging can slice the same ranking.
  recs-api's recommendation cache should hold a longer list per (user, model, genres) so later
  pages are cheap; ratings made while scrolling invalidate it (existing debounce).
- UI: IntersectionObserver sentinel, keyboard/screen-reader friendly "Load more" fallback, stop
  at a sensible depth (a few hundred) rather than the whole catalog.

## 2. Want-to-watch list

- New `watchlist` table owned by recs-api (user_id, movie_id, added_at), endpoints to add,
  remove and list, and a "Want to watch" toggle on cards and the movie page; a My watchlist page.
- Decide how it feeds recommendations: exclude watchlisted movies from For you (likely), and/or
  treat them as a weak positive signal (fits implicit ALS). Watchlist data stays in app.db; if it
  is ever exported for training, the export rules (no identity) apply as for ratings.
- Account deletion must remove it (spec: delete-me removes everything).

## 3. Richer movie page

**Built 2026-11-12** (branch `feat/movie-details`): `GET /api/movies/{id}/details` with plot summary,
tagline, runtime, release date, directors and cast. The notes below are the original idea.

- Overview/description, runtime, release date, cast/crew highlights, from TMDB (MovieLens
  `links.csv` already maps movieId → tmdbId; recs-api already calls TMDB for posters with the key
  from the environment). Cache responses on disk like posters, with a TTL, so TMDB is not called
  per page view; respect TMDB rate limits and terms (open audit item SA5-6: attribution/logo).
- Only recs-api talks to TMDB (the single allowed outbound call); any new fields go through
  public.yaml. Treat TMDB text as untrusted (rendered as text, never HTML).

## 4. Group recommendations

- Users create a group and invite others (group invite codes, like signup invites); members can
  leave; the owner can remove members or delete the group.
- recs-api assembles each member's ratings and asks recs-model for a group list. recs-model still
  never sees identity: it gets anonymous rating lists (one per member) and an aggregation strategy.
- Aggregation to choose and compare: average of members' scores, least misery (maximise the
  minimum), and a merged-profile fold-in. Exclude movies any member has rated (seen); show per
  member "why" without revealing other members' individual ratings unless they opt in.
- Needs: new tables (groups, memberships, group invites), new public and model endpoints,
  rate limits, privacy rules in the spec, and a delta security audit (authorization between
  members is the main risk).
