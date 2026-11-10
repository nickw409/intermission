// Poster images come only from recs-api's proxy on this origin (GET /api/posters/{movieId}); the
// browser never talks to TMDB. The URL is built from a validated integer id, never from text.

const MAX_MOVIE_ID = 2147483647; // contracts/public.yaml MovieId maximum

export function posterUrl(movieId: number): string {
  if (!Number.isInteger(movieId) || movieId < 1 || movieId > MAX_MOVIE_ID) {
    throw new Error(`invalid movie id ${movieId}`);
  }
  return `/api/posters/${movieId}`;
}
