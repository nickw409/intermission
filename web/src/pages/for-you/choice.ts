// What the For you page shows: which model, and the filter: which genres (if any) and which range of
// release years (if any) the list is narrowed to.
//
// The view lives in the URL query (?model=knn&genres=Action,Horror&year_from=1990&year_to=1999) so
// it can be shared or bookmarked. A URL with any of those keys (even an empty one) is the whole view;
// the remembered choice is only used when the URL has none, and is never written into the URL on its
// own. Anything the contract would reject (an unknown genre, a year that is not a whole number from
// 1800 to 2100) or a model that is not loaded is dropped, never sent.
//
// Years: an unusable range drops BOTH bounds. That is a bound that is present but not a year
// (out of range, not a whole number) or an inverted range (year_from later than year_to); it is
// neither kept one-sided nor swapped. Such a range only comes from a hand-edited link or a bug, and
// guessing what it meant could show a list the user did not ask for; the unfiltered list is the
// honest fallback. An absent or empty bound is simply unbounded. (The year inputs never apply an
// unusable range in the first place; see Controls.tsx.)
//
// What is remembered (localStorage) is what the user chose: their genres and year range, and a model
// only if they picked one in the selector that is not the server's default (so a later default change
// applies). A model the page guessed (the model list was down) is never remembered or put in the URL.
import type { Schemas } from '../../api/client';
import type { ModelName, ModelsResponse } from '../../lib/models';

export type Genre = Schemas['Genre'];

/** Inclusive release years; null is unbounded on that side. Never inverted (see cleanYears). */
export interface YearRange {
  from: number | null;
  to: number | null;
}

/** What the list is narrowed to. */
export interface Filter {
  /** Distinct genres, in contract (enum) order. Empty means no genre filter. */
  genres: Genre[];
  years: YearRange;
}

export interface Choice extends Filter {
  model: ModelName;
}

/** A choice read from the URL or storage, before checking it against the loaded models. */
export interface RawChoice extends Filter {
  model: string | null;
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

// ---- Years ----

// The contract's bounds for year_from / year_to.
export const MIN_YEAR = 1800;
export const MAX_YEAR = 2100;

export const NO_YEARS: YearRange = { from: null, to: null };

/**
 * A year from the URL, storage or an input: a whole number from 1800 to 2100, written as exactly
 * four digits if it is a string. Anything else (including '' and half-typed values) is null.
 */
export function parseYear(value: unknown): number | null {
  let year: number;
  if (typeof value === 'number') year = value;
  else if (typeof value === 'string' && /^\d{4}$/.test(value)) year = Number(value);
  else return null;
  return Number.isInteger(year) && year >= MIN_YEAR && year <= MAX_YEAR ? year : null;
}

/**
 * A range from two raw bounds (URL or storage values). An absent or empty bound is unbounded; a
 * present bound that is not a year, or an inverted range, drops both (see the note at the top).
 */
export function cleanYears(rawFrom: unknown, rawTo: unknown): YearRange {
  const absent = (raw: unknown) => raw === null || raw === undefined || raw === '';
  const from = absent(rawFrom) ? null : parseYear(rawFrom);
  const to = absent(rawTo) ? null : parseYear(rawTo);
  if (from === null && !absent(rawFrom)) return NO_YEARS;
  if (to === null && !absent(rawTo)) return NO_YEARS;
  if (from !== null && to !== null && from > to) return NO_YEARS;
  return { from, to };
}

export function hasYears(years: YearRange): boolean {
  return years.from !== null || years.to !== null;
}

/** Short form for the summary line: "1990–1999", "1995", "from 1990", "up to 1999", or ''. */
export function yearPhrase({ from, to }: YearRange): string {
  if (from !== null && to !== null) return from === to ? `${from}` : `${from}–${to}`;
  if (from !== null) return `from ${from}`;
  if (to !== null) return `up to ${to}`;
  return '';
}

/** Sentence form: "released 1990–1999", "released in 1995", "released 1990 or later", "released
 * 1999 or earlier", or ''. */
export function releasedPhrase({ from, to }: YearRange): string {
  if (from !== null && to !== null) return from === to ? `released in ${from}` : `released ${from}–${to}`;
  if (from !== null) return `released ${from} or later`;
  if (to !== null) return `released ${to} or earlier`;
  return '';
}

// ---- URL ----

const VIEW_KEYS = ['model', 'genres', 'year_from', 'year_to'];

/** The genres and years in a query string (the page URL or a debounced filter key). */
export function parseFilter(params: URLSearchParams): Filter {
  return {
    genres: parseGenres(params.get('genres') ?? ''),
    years: cleanYears(params.get('year_from'), params.get('year_to')),
  };
}

/** The filter as query parameters, each only when set. */
export function filterParams(filter: Filter): URLSearchParams {
  const params = new URLSearchParams();
  const genres = genresParam(filter.genres);
  if (genres) params.set('genres', genres);
  if (filter.years.from !== null) params.set('year_from', String(filter.years.from));
  if (filter.years.to !== null) params.set('year_to', String(filter.years.to));
  return params;
}

/** The filter as /api/recommend query fields, each only when set. */
export function filterQuery(filter: Filter): { genres?: string; year_from?: number; year_to?: number } {
  const genres = genresParam(filter.genres);
  return {
    ...(genres ? { genres } : {}),
    ...(filter.years.from !== null ? { year_from: filter.years.from } : {}),
    ...(filter.years.to !== null ? { year_to: filter.years.to } : {}),
  };
}

/** The view the URL asks for, or null when it has none of the view keys (use the remembered choice). */
export function fromSearchParams(params: URLSearchParams): RawChoice | null {
  if (!VIEW_KEYS.some((key) => params.has(key))) return null;
  return { model: params.get('model'), ...parseFilter(params) };
}

/** The URL for a view. `withModel` is false when the model is only a guess. */
export function toSearchParams(choice: Choice, withModel: boolean): URLSearchParams {
  const params = new URLSearchParams();
  if (withModel) params.set('model', choice.model);
  for (const [key, value] of filterParams(choice)) params.set(key, value);
  return params;
}

// ---- Storage ----

export const STORAGE_KEY = 'intermission.for-you';

/** The remembered choice, or nothing if storage is unavailable or holds something unexpected. */
// Stored as { model?, genres, year_from?, year_to? }; values saved before the year range existed
// have no year keys and read as no range.
export function readStored(): RawChoice {
  const empty: RawChoice = { model: null, genres: [], years: NO_YEARS };
  try {
    const value: unknown = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null');
    if (value === null || typeof value !== 'object' || Array.isArray(value)) return empty;
    const { model, genres, year_from, year_to } = value as Record<string, unknown>;
    return {
      model: typeof model === 'string' ? model : null,
      genres: Array.isArray(genres) ? cleanGenres(genres) : [],
      years: cleanYears(year_from, year_to),
    };
  } catch {
    return empty; // storage unavailable or malformed JSON
  }
}

/** Remembers a choice (`model: null` = the default). Losing it (private mode, full storage) is harmless. */
export function writeStored(choice: RawChoice): void {
  const value = {
    ...(choice.model === null ? {} : { model: choice.model }),
    genres: choice.genres,
    ...(choice.years.from === null ? {} : { year_from: choice.years.from }),
    ...(choice.years.to === null ? {} : { year_to: choice.years.to }),
  };
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
  return { model, genres: source.genres, years: source.years };
}
