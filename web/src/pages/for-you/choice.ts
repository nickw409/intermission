// What the For you page shows: which model, and which genres (if any) the list is narrowed to.
//
// The choice lives in the URL query (?model=knn&genres=Action,Horror) so a view can be shared or
// bookmarked, and the last choice the user made is remembered in localStorage. When the page opens,
// each part comes from the URL if it is valid there, else from storage if valid there, else the
// default. Anything not in the contract's enums (or a model that is not loaded) is dropped, never
// sent to the API.
import type { Schemas } from '../../api/client';
import type { ModelName, ModelsResponse } from '../../lib/models';

export type Genre = Schemas['Genre'];

export interface Choice {
  model: ModelName;
  /** Distinct genres, in contract (enum) order. Empty means no filter. */
  genres: Genre[];
}

/** A choice read from the URL or storage, before checking it against the loaded models. */
interface RawChoice {
  model: string | null;
  genres: Genre[];
}

// Contract order: the API gets genres in this order. Record<Genre, …> makes a new genre in the
// contract a type error until it is listed here, with the phrase used in sentences.
const GENRE_PHRASES: Record<Genre, string> = {
  Action: 'action',
  Adventure: 'adventure',
  Animation: 'animation',
  Children: "children's",
  Comedy: 'comedy',
  Crime: 'crime',
  Documentary: 'documentary',
  Drama: 'drama',
  Fantasy: 'fantasy',
  'Film-Noir': 'film noir',
  Horror: 'horror',
  IMAX: 'IMAX',
  Musical: 'musical',
  Mystery: 'mystery',
  Romance: 'romance',
  'Sci-Fi': 'sci-fi',
  Thriller: 'thriller',
  War: 'war',
  Western: 'western',
};

export const GENRES = Object.keys(GENRE_PHRASES) as Genre[];

/** Chip order: the contract's order, except IMAX (a screen format, not a genre) goes last. */
export const CHIP_ORDER: Genre[] = [...GENRES.filter((g) => g !== 'IMAX'), 'IMAX'];

function isGenre(value: unknown): value is Genre {
  return typeof value === 'string' && Object.hasOwn(GENRE_PHRASES, value);
}

/** Keeps the known genres, once each, in contract order. */
export function cleanGenres(values: readonly unknown[]): Genre[] {
  return GENRES.filter((g) => values.includes(g));
}

/** The `genres` query value for the API, or undefined for no filter. */
export function genresParam(genres: readonly Genre[]): string | undefined {
  return genres.length > 0 ? cleanGenres(genres).join(',') : undefined;
}

/** "horror", "horror or action", "action, horror or sci-fi". */
export function genrePhrase(genres: readonly Genre[]): string {
  const words = cleanGenres(genres).map((g) => GENRE_PHRASES[g]);
  if (words.length <= 1) return words.join('');
  return `${words.slice(0, -1).join(', ')} or ${words.at(-1)}`;
}

// ---- URL ----

export function fromSearchParams(params: URLSearchParams): RawChoice {
  return { model: params.get('model'), genres: cleanGenres((params.get('genres') ?? '').split(',')) };
}

export function toSearchParams(choice: Choice): URLSearchParams {
  const params = new URLSearchParams({ model: choice.model });
  const genres = genresParam(choice.genres);
  if (genres) params.set('genres', genres);
  return params;
}

// ---- Storage ----

export const STORAGE_KEY = 'intermission.for-you';

/** The remembered choice, or nothing if storage is unavailable or holds something unexpected. */
export function readStored(): RawChoice {
  const empty: RawChoice = { model: null, genres: [] };
  let raw: string | null;
  try {
    raw = localStorage.getItem(STORAGE_KEY);
  } catch {
    return empty;
  }
  if (raw === null) return empty;
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return empty;
  }
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return empty;
  const { model, genres } = value as { model?: unknown; genres?: unknown };
  return {
    model: typeof model === 'string' ? model : null,
    genres: Array.isArray(genres) ? cleanGenres(genres.filter(isGenre)) : [],
  };
}

/** Remembers the choice for next time. Losing it (private mode, full storage) is harmless. */
export function writeStored(choice: Choice): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ model: choice.model, genres: cleanGenres(choice.genres) }));
  } catch {
    // Storage unavailable: the URL still carries the choice.
  }
}

// ---- Resolving ----

/**
 * A model the page may ask for. With a model list, only loaded models count. Without one (the
 * call failed) nothing from the URL or storage is trusted and the caller's fallback is used.
 */
function usableModel(model: string | null, models: ModelsResponse | null): ModelName | null {
  if (model === null || models === null) return null;
  return models.models.find((m) => m === model) ?? null;
}

/** URL first, then storage, then `fallback`; each part separately. */
export function resolveChoice(
  url: RawChoice,
  stored: RawChoice,
  models: ModelsResponse | null,
  fallback: ModelName,
): Choice {
  const model = usableModel(url.model, models) ?? usableModel(stored.model, models) ?? fallback;
  const genres = url.genres.length > 0 ? url.genres : stored.genres;
  return { model, genres };
}
