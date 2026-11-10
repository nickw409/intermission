import { act, renderHook, waitFor } from '@testing-library/react';
import { http, HttpResponse } from 'msw';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { server } from '../test/setup';
import { notifyRatingSaved, onRatingSaved, saveRating, useOptimisticRatings, useRefetchAfterRating } from './ratings';

function deferredPut() {
  const releases: Array<(r: Response) => void> = [];
  server.use(http.put('/api/ratings/:movieId', () => new Promise<Response>((resolve) => releases.push(resolve))));
  return releases;
}

const ok = (movieId: number, rating: number) =>
  HttpResponse.json({ movieId, title: 'x', year: 2000, rating, updated_at: 1 });
const fail = () => HttpResponse.json({ error: { code: 'internal', message: 'boom' } }, { status: 500 });

describe('saveRating', () => {
  it('PUTs a value, DELETEs null, and tells listeners after the server accepts it', async () => {
    const seen: string[] = [];
    server.events.on('request:start', ({ request }) => {
      seen.push(`${request.method} ${new URL(request.url).pathname}`);
    });
    const listener = vi.fn();
    const off = onRatingSaved(listener);
    await saveRating(4993, 4.5);
    await saveRating(4993, null);
    off();
    expect(seen).toEqual(['PUT /api/ratings/4993', 'DELETE /api/ratings/4993']);
    expect(listener.mock.calls).toEqual([[{ movieId: 4993, rating: 4.5 }], [{ movieId: 4993, rating: null }]]);
  });

  it('does not notify listeners when the save fails', async () => {
    server.use(http.put('/api/ratings/:movieId', fail));
    const listener = vi.fn();
    const off = onRatingSaved(listener);
    await expect(saveRating(1, 3)).rejects.toThrow();
    off();
    expect(listener).not.toHaveBeenCalled();
  });
});

describe('useOptimisticRatings', () => {
  it('shows the new value at once and keeps it when the save succeeds', async () => {
    const releases = deferredPut();
    const { result } = renderHook(() => useOptimisticRatings());
    let done: Promise<boolean> = Promise.resolve(false);
    act(() => {
      done = result.current.rate(1, 4, null);
    });
    expect(result.current.ratingOf(1, null)).toBe(4);
    await waitFor(() => expect(releases).toHaveLength(1));
    await act(async () => {
      releases[0]!(ok(1, 4));
      expect(await done).toBe(true);
    });
    expect(result.current.ratingOf(1, null)).toBe(4);
    expect(result.current.errorOf(1)).toBeUndefined();
  });

  it('rolls back to the last saved value and reports an error when the save fails', async () => {
    const releases = deferredPut();
    const { result } = renderHook(() => useOptimisticRatings());
    let done: Promise<boolean> = Promise.resolve(true);
    act(() => {
      done = result.current.rate(1, 2, 3.5);
    });
    expect(result.current.ratingOf(1, 3.5)).toBe(2);
    await waitFor(() => expect(releases).toHaveLength(1));
    await act(async () => {
      releases[0]!(fail());
      expect(await done).toBe(false);
    });
    expect(result.current.ratingOf(1, 3.5)).toBe(3.5);
    expect(result.current.errorOf(1)).toMatch(/couldn't save/i);
  });

  it('a late failure of an older save does not undo a newer one', async () => {
    const releases = deferredPut();
    const { result } = renderHook(() => useOptimisticRatings());
    let first: Promise<boolean> = Promise.resolve(true);
    let second: Promise<boolean> = Promise.resolve(true);
    act(() => {
      first = result.current.rate(1, 2, null);
    });
    act(() => {
      second = result.current.rate(1, 5, null);
    });
    await waitFor(() => expect(releases).toHaveLength(2));
    await act(async () => {
      releases[1]!(ok(1, 5));
      await second;
      releases[0]!(fail());
      await first;
    });
    expect(result.current.ratingOf(1, null)).toBe(5);
    expect(result.current.errorOf(1)).toBeUndefined();
  });
});

describe('useRefetchAfterRating', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('calls back once, 500 ms after the last saved rating change', () => {
    vi.useFakeTimers();
    const refetch = vi.fn();
    renderHook(() => useRefetchAfterRating(refetch));
    act(() => {
      notifyRatingSaved({ movieId: 1, rating: 4 });
      vi.advanceTimersByTime(300);
      notifyRatingSaved({ movieId: 2, rating: 3 });
      vi.advanceTimersByTime(499);
    });
    expect(refetch).not.toHaveBeenCalled();
    act(() => {
      vi.advanceTimersByTime(1);
    });
    expect(refetch).toHaveBeenCalledTimes(1);
    act(() => {
      vi.advanceTimersByTime(5000);
    });
    expect(refetch).toHaveBeenCalledTimes(1);
  });

  it('cancels a pending refetch when the component unmounts', () => {
    vi.useFakeTimers();
    const refetch = vi.fn();
    const { unmount } = renderHook(() => useRefetchAfterRating(refetch));
    act(() => notifyRatingSaved({ movieId: 1, rating: 4 }));
    unmount();
    act(() => {
      vi.advanceTimersByTime(1000);
    });
    expect(refetch).not.toHaveBeenCalled();
  });
});
