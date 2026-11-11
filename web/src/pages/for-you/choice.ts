// What the For you page shows: which model, and which genres (if any) the list is narrowed to.
//
// The view lives in the URL query (?model=knn&genres=Action,Horror) so it can be shared or
// bookmarked. A URL with a `model` or `genres` key (even an empty one) is the whole view; the
// remembered choice is only used when the URL has neither, and is never written into the URL on its
// own. Anything not in the contract's enums (or a model that is not loaded) is dropped, never sent.
//
// What is remembered (localStorage) is what the user chose: their genres, and a model only if they
// picked one in the selector that is not the server's default (so a later default change applies).
// A model the page guessed (the model list was down) is never remembered or put in the URL.
import type { Schemas } from '../../api/client';
import type { ModelName, ModelsResponse } from '../../lib/models';

export type Genre = Schemas['Genre'];

export interface Choice {
  model: ModelName;
  /** Distinct genres, in contract (enum) order. Empty means no filter. */
  genres: Genre[];
}

/** A choice read from the URL or storage, before checking it against the loaded models. */
export interface RawChoice {
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

/** Keeps the known genres, once each, in contract order. */
export function cleanGenres(values: readonly unknown[]): Genre[] {
  return GENRES.filter((g) => values.includes(g));
}

/** The `genres` query value for the API: comma-joined in contract order, '' for no filter. */
export function genresParam(genres: readonly Genre[]): string {
  return cleanGenres(genres).join(',');
}

export function parseGenres(param: string): Genre[] {
  return cleanGenres(param.split(','));
}

/** "horror", "action or horror", "action, horror or sci-fi". */
export function genrePhrase(genres: readonly Genre[]): string {
  const words = cleanGenres(genres).map((g) => GENRE_PHRASES[g]);
  if (words.length <= 1) return words.join('');
  return `${words.slice(0, -1).join(', ')} or ${words.at(-1)}`;
}

// ---- URL ----

/** The view the URL asks for, or null when it has neither key (use the remembered choice). */
export function fromSearchParams(params: URLSearchParams): RawChoice | null {
  if (!params.has('model') && !params.has('genres')) return null;
  return { model: params.get('model'), genres: parseGenres(params.get('genres') ?? '') };
}

/** The URL for a view. `withModel` is false when the model is only a guess. */
export function toSearchParams(choice: Choice, withModel: boolean): URLSearchParams {
  const params = new URLSearchParams();
  if (withModel) params.set('model', choice.model);
  const genres = genresParam(choice.genres);
  if (genres) params.set('genres', genres);
  return params;
}

// ---- Storage ----

export const STORAGE_KEY = 'intermission.for-you';

/** The remembered choice, or nothing if storage is unavailable or holds something unexpected. */
export function readStored(): RawChoice {
  const empty: RawChoice = { model: null, genres: [] };
  try {
    const value: unknown = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null');
    if (value === null || typeof value !== 'object' || Array.isArray(value)) return empty;
    const { model, genres } = value as { model?: unknown; genres?: unknown };
    return {
      model: typeof model === 'string' ? model : null,
      genres: Array.isArray(genres) ? cleanGenres(genres) : [],
    };
  } catch {
    return empty; // storage unavailable or malformed JSON
  }
}

/** Remembers a choice (`model: null` = the default). Losing it (private mode, full storage) is harmless. */
export function writeStored(choice: RawChoice): void {
  const value = choice.model === null ? { genres: choice.genres } : { model: choice.model, genres: choice.genres };
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(value));
  } catch {
    // Storage unavailable: the URL still carries the view.
  }
}

// ---- Resolving ----

/**
 * The view to show: the URL's if it has one, else the remembered one. The model must be loaded;
 * without a model list (the call failed) none is trusted and `fallback` is used.
 */
export function resolveChoice(
  url: RawChoice | null,
  stored: RawChoice,
  models: ModelsResponse | null,
  fallback: ModelName,
): Choice {
  const source = url ?? stored;
  const model = models?.models.find((m) => m === source.model) ?? fallback;
  return { model, genres: source.genres };
}
