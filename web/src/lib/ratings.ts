// Saving ratings, optimistic updates with rollback, and the "refetch recommendations after a
// rating change" rule (spec, Frontend rules).
//
//   saveRating(id, 4.5)  PUT /api/ratings/{id};  saveRating(id, null)  DELETE /api/ratings/{id}.
//   After the server accepts a change, every onRatingSaved listener hears about it.
//
//   useOptimisticRatings() keeps a per-movie override on top of whatever the page loaded:
//   rate() shows the new value at once; if the save fails the movie goes back to its last saved
//   value and errorOf(id) explains why. Only the newest save for a movie may roll it back, so a
//   slow failure can't undo a later success.
//
//   useRefetchAfterRating(fn) calls fn once, 500 ms after the last saved change. The timer starts
//   when the server has stored the rating, so the refetch always sees it.
import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../api/client';
import { friendlyMessage } from './errors';

export type RatingValue = number | null;

export interface RatingChange {
  movieId: number;
  rating: RatingValue;
}

type Listener = (change: RatingChange) => void;
const listeners = new Set<Listener>();

/** Subscribes to saved rating changes. Returns an unsubscribe function. */
export function onRatingSaved(listener: Listener): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/** Tells every listener that a rating change was stored. saveRating calls this itself. */
export function notifyRatingSaved(change: RatingChange): void {
  for (const listener of [...listeners]) listener(change);
}

export async function saveRating(movieId: number, rating: RatingValue): Promise<void> {
  if (rating === null) {
    await api.delete('/api/ratings/{movieId}', { path: { movieId } });
  } else {
    await api.put('/api/ratings/{movieId}', { path: { movieId }, body: { rating } });
  }
  notifyRatingSaved({ movieId, rating });
}

export function ratingErrorMessage(error: unknown): string {
  return `Couldn't save your rating. ${friendlyMessage(error)}`;
}

export interface OptimisticRatings {
  /** The value to show: the local override if there is one, else `loaded`. */
  ratingOf: (movieId: number, loaded: RatingValue) => RatingValue;
  /** Why the last save for this movie failed, if it did. */
  errorOf: (movieId: number) => string | undefined;
  /** Shows `next` at once and saves it. `current` is the value shown before. Resolves to success. */
  rate: (movieId: number, next: RatingValue, current: RatingValue) => Promise<boolean>;
  /** Movies whose value differs from what was loaded (newest value, possibly unsaved). */
  overrides: ReadonlyMap<number, RatingValue>;
}

export function useOptimisticRatings(): OptimisticRatings {
  const [overrides, setOverrides] = useState<ReadonlyMap<number, RatingValue>>(new Map());
  const [errors, setErrors] = useState<ReadonlyMap<number, string>>(new Map());
  const saved = useRef(new Map<number, RatingValue>()); // last value the server accepted
  const latest = useRef(new Map<number, number>()); // newest save attempt per movie
  const counter = useRef(0);
  // A save can settle after the page unmounts (the user navigated away); drop its state updates.
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const setOverride = (movieId: number, value: RatingValue) => {
    if (mounted.current) setOverrides((current) => new Map(current).set(movieId, value));
  };
  const setError = (movieId: number, message: string | undefined) => {
    if (!mounted.current) return;
    setErrors((current) => {
      const next = new Map(current);
      if (message === undefined) next.delete(movieId);
      else next.set(movieId, message);
      return next;
    });
  };

  const rate = useCallback(async (movieId: number, next: RatingValue, current: RatingValue) => {
    const attempt = ++counter.current;
    latest.current.set(movieId, attempt);
    if (!saved.current.has(movieId)) saved.current.set(movieId, current);
    setOverride(movieId, next);
    setError(movieId, undefined);
    try {
      await saveRating(movieId, next);
      saved.current.set(movieId, next);
      return true;
    } catch (error) {
      if (latest.current.get(movieId) === attempt) {
        setOverride(movieId, saved.current.get(movieId) ?? null);
        setError(movieId, ratingErrorMessage(error));
      }
      return false;
    }
  }, []);

  const ratingOf = useCallback(
    (movieId: number, loaded: RatingValue) => (overrides.has(movieId) ? (overrides.get(movieId) ?? null) : loaded),
    [overrides],
  );
  const errorOf = useCallback((movieId: number) => errors.get(movieId), [errors]);

  return { ratingOf, errorOf, rate, overrides };
}

export const REFETCH_DELAY_MS = 500;

export function useRefetchAfterRating(refetch: () => void, delayMs = REFETCH_DELAY_MS): void {
  const latest = useRef(refetch);
  useEffect(() => {
    latest.current = refetch;
  });

  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const off = onRatingSaved(() => {
      clearTimeout(timer);
      timer = setTimeout(() => latest.current(), delayMs);
    });
    return () => {
      off();
      clearTimeout(timer);
    };
  }, [delayMs]);
}
