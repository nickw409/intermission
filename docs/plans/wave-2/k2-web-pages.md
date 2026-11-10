# K2 — Web pages: onboarding, For you, Compare, Movie, My ratings, How it works

**Owns:** `web/src/pages/{onboarding,for-you,compare,movie,ratings,how-it-works}/`, `web/src/components/`
(new shared components; don't break StarRating's tested behaviour), `web/src/lib/` except `lib/reports/`,
`web/src/mocks/handlers.ts` (make `getReport` answer by run_id from the fixtures, not always als).
**Builds on:** K1 shell (session, client, StarRating, banner, styles tokens), L1 (Evaluation page — link to it).
**Contracts:** `contracts/public.yaml` (searchMovies, getMovie, getSimilar, getOnboarding, listRatings,
putRating, deleteRating, recommend, compare, listModels, getPoster), fixtures in `contracts/fixtures/public/`.
**Spec:** "Frontend" pages 2–6 and 8, and the rules (optimistic ratings with rollback, 500 ms debounced
refetch after a rating change, keyboard-operable ratings, poster alt text, TMDB attribution, no third-party
scripts).

## Pages
- **Onboarding:** grid of ~30 from `/api/onboarding`, StarRating per card, progress bar to 10 ratings,
  search box (debounced `/api/movies/search`) to rate any movie; "Continue" to For you at ≥ 10.
- **For you:** top 20 from the default model (`/api/models` `default_model`; fall back to `knn`/`pop`).
  Poster cards (`/api/posters/{id}`, alt = title + year) with "Because you rated X, Y", rate control and
  "Not interested" (hides the card locally; phase 6 sends feedback — leave a clearly marked hook).
  `cold_start: true` → show the popularity list with a prompt to rate more.
- **Compare models:** 3–5 columns from `/api/recommend/compare`, model picker (only models listed by
  `/api/models`), movies appearing in ≥ 2 columns highlighted (and announced textually, not colour only).
- **Movie page:** details (`/api/movies/{id}`), the user's rating control, similar-movies row with a
  knn/content toggle (`/api/movies/{id}/similar`).
- **My ratings:** sortable list (date, title, rating), edit inline (StarRating), delete with undo.
- **How it works:** short explanation of each model (from the spec primer), linking to its Evaluation results.

## Rules / gate tests (Vitest + MSW, axe on every page)
- Optimistic rating: UI updates immediately; on error rolls back and shows a message. Tested.
- After any rating change, recommendation lists refetch once, debounced 500 ms (fake timers). Tested.
- 503 model_unavailable on recommendation endpoints shows the banner; ratings pages keep working. Tested.
- Every page: loading, empty and error states; no axe violations.
- No dangerouslySetInnerHTML; poster images same-origin only.
