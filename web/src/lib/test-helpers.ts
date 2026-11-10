// Helpers shared by the page tests (never imported by app code).
import { act, fireEvent } from '@testing-library/react';
import { http, HttpResponse } from 'msw';
import { server } from '../test/setup';

/** Records every request to `pathname` (any method unless given). */
export function recordRequests(pathname: string | RegExp, method?: string): Request[] {
  const seen: Request[] = [];
  server.events.on('request:start', ({ request }) => {
    const path = new URL(request.url).pathname;
    const matches = typeof pathname === 'string' ? path === pathname : pathname.test(path);
    if (matches && (!method || request.method === method)) seen.push(request.clone());
  });
  return seen;
}

/** Query parameters of a recorded request. */
export function queryOf(request: Request): Record<string, string> {
  return Object.fromEntries(new URL(request.url).searchParams);
}

/** Moves a StarRating slider to `value` with the keyboard and saves it (Home, arrows, Enter). */
export function rateWithKeyboard(slider: HTMLElement, value: number): void {
  act(() => slider.focus());
  fireEvent.keyDown(slider, { key: 'Home' });
  for (let v = 0.5; v < value; v += 0.5) fireEvent.keyDown(slider, { key: 'ArrowRight' });
  fireEvent.keyDown(slider, { key: 'Enter' });
}

export const errorBody = (code: string, status: number) =>
  HttpResponse.json({ error: { code, message: code } }, { status });

/** Makes every endpoint that needs recs-model answer 503 model_unavailable. */
export function modelServiceDown(): void {
  const down = () => errorBody('model_unavailable', 503);
  server.use(
    http.get('/api/models', down),
    http.get('/api/recommend', down),
    http.get('/api/recommend/compare', down),
    http.get('/api/onboarding', down),
    http.get('/api/movies/search', down),
    http.get('/api/movies/:movieId', down),
    http.get('/api/movies/:movieId/similar', down),
  );
}

/** A response that waits until `release()` is called. */
export function gate(): { wait: Promise<void>; release: () => void } {
  let release: () => void = () => {};
  const wait = new Promise<void>((resolve) => {
    release = resolve;
  });
  return { wait, release };
}
