import { http, HttpResponse } from 'msw';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { fixtureFor, handlerFor } from '../mocks/handlers';
import { server } from '../test/setup';
import { api, ApiError } from './client';

afterEach(() => vi.restoreAllMocks());

describe('api client', () => {
  it('sends same-origin credentials, X-Requested-With and a JSON body', async () => {
    const fetchSpy = vi.spyOn(globalThis, 'fetch');
    let seen: Request | undefined;
    let seenBody: unknown;
    server.use(
      http.put('/api/ratings/:movieId', async ({ request }) => {
        seen = request.clone();
        seenBody = await request.json();
        return HttpResponse.json({ movieId: 4993, title: 'x', year: 2001, rating: 4.5, updated_at: 0 });
      }),
    );

    const rating = await api.put('/api/ratings/{movieId}', { path: { movieId: 4993 }, body: { rating: 4.5 } });

    expect(rating.rating).toBe(4.5);
    const init = fetchSpy.mock.calls[0]?.[1];
    expect(init?.credentials).toBe('same-origin');
    expect(seen?.method).toBe('PUT');
    expect(new URL(seen?.url ?? '').pathname).toBe('/api/ratings/4993');
    expect(seen?.headers.get('X-Requested-With')).toBe('recs');
    expect(seen?.headers.get('Content-Type')).toBe('application/json');
    expect(seenBody).toEqual({ rating: 4.5 });
  });

  it('serialises query parameters and returns the typed body', async () => {
    const result = await api.get('/api/recommend', { query: { model: 'als', k: 3 } });
    expect(result.model).toBe('als');
    expect(result.items[0]?.movieId).toBe(4993);
  });

  it('returns undefined for 204 responses', async () => {
    await expect(api.delete('/api/ratings/{movieId}', { path: { movieId: 4993 } })).resolves.toBeUndefined();
  });

  it('parses contract error bodies into ApiError', async () => {
    server.use(handlerFor('putRating', 'too_many_ratings'));
    const expected = fixtureFor('putRating').cases.find((c) => c.name === 'too_many_ratings');
    const error = await api
      .put('/api/ratings/{movieId}', { path: { movieId: 4993 }, body: { rating: 4.5 } })
      .catch((e: unknown) => e);
    expect(error).toBeInstanceOf(ApiError);
    expect(error).toMatchObject({ status: 409, code: 'too_many_ratings', message: (expected?.response.body as { error: { message: string } }).error.message });
  });

  it('maps non-contract error bodies to code internal', async () => {
    server.use(http.get('/api/me', () => new HttpResponse('<html>Bad gateway</html>', { status: 502 })));
    await expect(api.get('/api/me')).rejects.toMatchObject({ status: 502, code: 'internal' });
  });
});
