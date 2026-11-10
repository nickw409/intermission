# K1 — Web app shell, auth pages, design basics, star rating, API error handling

**Owns:** `web/src/` EXCEPT `web/src/pages/evaluation/`, `web/src/lib/reports/` (L1) and the wiring
file `web/src/routes.tsx` (already lists every page). Page directories `web/src/pages/<name>/`
other than login/signup are placeholders you may leave as they are (K2 builds them in Wave 2).
**Contract:** `contracts/public.yaml`; generated types `web/src/api/schema.d.ts` (never hand-edit;
`npm run gen:api`). MSW mocks come from `contracts/fixtures/public/`.
**Spec:** "Frontend" (rules), pages 1 (sign up / log in).

## Gate tests (write first; Vitest + Testing Library + MSW)
- **StarRating** component: 0.5-step values 0.5–5.0; fully keyboard operable (arrow keys change by
  0.5, Home/End, Enter/Space commits; it's a single focusable `role="slider"` or radio group with
  correct aria-valuenow/valuetext like "3.5 stars"); clearing a rating is possible; readonly mode.
- **Login/Signup pages:** field validation mirrors the contract (username 3–32 `[A-Za-z0-9_.-]`,
  password 10–128; the invite code field is always shown, with helper text "Required while signup
  is invite-only"); server errors map to friendly messages
  (`invalid_credentials`, `invalid_invite`, `username_taken`, `rate_limited`); successful login
  navigates to `/onboarding` if the user has < 10 ratings else `/`.
- **Auth state:** `GET /api/me` on load; 401 → logged-out nav; logout calls POST /api/auth/logout.
- **Model-unavailable banner:** any API call returning 503 `model_unavailable` shows a dismissible
  site-wide banner ("Recommendations are temporarily unavailable"); other pages keep working.
- **Layout:** header nav (For you, Compare, My ratings, Evaluation, How it works, account menu),
  footer with GroupLens credit ("Data from MovieLens by GroupLens Research; not endorsed by the
  University of Minnesota or GroupLens") and TMDB attribution text ("This product uses the TMDB API
  but is not endorsed or certified by TMDB"). No dataset or model downloads anywhere.
- Every fetch sends `credentials: 'same-origin'` and `X-Requested-With: recs` (already in client.ts — keep a test).
- An axe-core accessibility check (vitest-axe or @axe-core/react in tests) on the login page and layout: no violations.

## Notes
- Plain CSS with CSS variables (light/dark via `prefers-color-scheme`); no UI kit needed. Clean,
  calm "cinema" look is welcome but keep it simple. No remote fonts, no CDNs.
- A small `useApi`/data-fetching helper is fine; don't add a heavy state library.
