// Poster images come only from recs-api's proxy on this origin (GET /api/posters/{movieId}); the
// browser never talks to TMDB. The URL is built from a validated integer id, never from text.

const MAX_MOVIE_ID = 2147483647; // contracts/public.yaml MovieId maximum

/** True for an id the poster endpoint accepts (a positive 32-bit integer). */
export function isMovieId(movieId: number): boolean {
  return Number.isInteger(movieId) && movieId >= 1 && movieId <= MAX_MOVIE_ID;
}

export function posterUrl(movieId: number): string {
  if (!isMovieId(movieId)) {
    throw new Error(`invalid movie id ${movieId}`);
  }
  return `/api/posters/${movieId}`;
}
