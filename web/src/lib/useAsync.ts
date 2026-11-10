// Loads data for a page section and tracks loading / error / ready.
//
//   const load = useCallback((signal) => api.get('/api/ratings', { signal }), []);
//   const { state, reload, update } = useAsync(load);
//
// When `load` changes (new arguments) the section shows 'loading' again until the new answer
// arrives, and older answers are ignored. `reload()` retries from scratch; `reload({ quiet: true })`
// keeps the current data on screen while it refreshes (used after a rating change).
import { useCallback, useEffect, useState } from 'react';
import { friendlyMessage } from './errors';

export type AsyncState<T> =
  | { status: 'loading' }
  | { status: 'error'; error: unknown; message: string }
  | { status: 'ready'; data: T; refreshing: boolean };

type Stored<T> =
  | { status: 'loading'; load: unknown }
  | { status: 'error'; load: unknown; error: unknown; message: string }
  | { status: 'ready'; load: unknown; data: T; refreshing: boolean };

export function useAsync<T>(load: (signal: AbortSignal) => Promise<T>) {
  const [stored, setStored] = useState<Stored<T>>({ status: 'loading', load });
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    const controller = new AbortController();
    load(controller.signal)
      .then((data) => {
        if (!controller.signal.aborted) setStored({ status: 'ready', load, data, refreshing: false });
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) setStored({ status: 'error', load, error, message: friendlyMessage(error) });
      });
    return () => controller.abort();
  }, [load, attempt]);

  const reload = useCallback(
    ({ quiet = false }: { quiet?: boolean } = {}) => {
      setStored((current) =>
        quiet && current.status === 'ready' ? { ...current, refreshing: true } : { status: 'loading', load },
      );
      setAttempt((n) => n + 1);
    },
    [load],
  );

  /** Changes the loaded data in place (optimistic edits). */
  const update = useCallback((change: (data: T) => T) => {
    setStored((current) => (current.status === 'ready' ? { ...current, data: change(current.data) } : current));
  }, []);

  let state: AsyncState<T>;
  if (stored.load !== load || stored.status === 'loading') state = { status: 'loading' };
  else if (stored.status === 'error') state = { status: 'error', error: stored.error, message: stored.message };
  else state = { status: 'ready', data: stored.data, refreshing: stored.refreshing };

  return { state, reload, update };
}
