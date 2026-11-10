import { act, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { errorBody, gate, modelServiceDown, queryOf, rateWithKeyboard, recordRequests } from '../../lib/test-helpers';
import { handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const LOTR = 'The Lord of the Rings: The Fellowship of the Ring';

/** Serves a ratings list with `count` entries (ids 9001…). */
function serveRatings(count: number) {
  const items = Array.from({ length: count }, (_, i) => ({
    movieId: 9001 + i,
    title: `Movie ${i}`,
    year: 2000,
    rating: 4,
    updated_at: 1794256000 - i,
  }));
  server.use(http.get('/api/ratings', () => HttpResponse.json({ items })));
}

async function openPage() {
  const app = renderApp('/onboarding');
  await screen.findByRole('heading', { level: 2, name: LOTR });
  await screen.findByText('5 of 10 rated');
  return app;
}

function progress() {
  return screen.getByRole('progressbar', { name: 'Ratings so far' });
}

function card(title: string): HTMLElement {
  const article = screen.getByRole('heading', { name: title }).closest('article');
  if (!article) throw new Error(`no card for ${title}`);
  return article;
}

/**
 * Fake timers from here on (call after the page has loaded), so the search debounce is driven
 * explicitly. Testing Library only advances fake timers while it waits if it sees a `jest` global.
 */
function fakeClock() {
  vi.useFakeTimers();
  vi.stubGlobal('jest', { advanceTimersByTime: (ms: number) => vi.advanceTimersByTime(ms) });
  return userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
}

function realClock() {
  vi.useRealTimers();
  vi.unstubAllGlobals();
}

describe('Onboarding page', () => {
  afterEach(realClock);

  it('shows loading states while the suggestions load', async () => {
    const { wait, release } = gate();
    server.use(
      http.get('/api/onboarding', async () => {
        await wait;
        return HttpResponse.json({ items: [] });
      }),
    );
    renderApp('/onboarding');
    expect(await screen.findByText('Loading movies to rate…')).toBeInTheDocument();
    expect(screen.getByRole('searchbox', { name: 'Search for any movie' })).toBeInTheDocument();
    release();
    expect(await screen.findByText(/rated everything we'd suggest/)).toBeInTheDocument();
    expect(screen.queryByText('Loading movies to rate…')).not.toBeInTheDocument();
  });

  it('shows "Searching…" while a search is in flight', async () => {
    const { wait, release } = gate();
    server.use(
      http.get('/api/movies/search', async () => {
        await wait;
        return HttpResponse.json({ items: [] });
      }),
    );
    await openPage();
    const user = fakeClock();
    await user.type(screen.getByRole('searchbox', { name: 'Search for any movie' }), 'x');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(300);
    });
    expect(await screen.findByText('Searching…')).toBeInTheDocument();
    release();
    expect(await screen.findByText('No movies match “x”.')).toBeInTheDocument();
  });

  it('still saves ratings after the model service goes down', async () => {
    const puts = recordRequests('/api/ratings/4993', 'PUT');
    await openPage();
    modelServiceDown();
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4);
    await waitFor(() => expect(puts).toHaveLength(1));
    await act(async () => {});
    expect(slider).toHaveAttribute('aria-valuetext', '4 stars');
    expect(within(card(LOTR)).queryByRole('alert')).not.toBeInTheDocument();
    expect(screen.getByText('6 of 10 rated')).toBeInTheDocument();
  });

  it('asks for ~30 popular movies and shows each as a poster card with a rating control', async () => {
    const onboarding = recordRequests('/api/onboarding');
    renderApp('/onboarding');
    expect(screen.getByRole('heading', { level: 1, name: 'Rate some movies' })).toBeInTheDocument();
    await screen.findByRole('heading', { level: 2, name: LOTR });
    expect(queryOf(onboarding[0]!)).toEqual({ n: '30' });
    const lotr = card(LOTR);
    expect(within(lotr).getByRole('img', { name: `${LOTR} (2001)` })).toHaveAttribute('src', '/api/posters/4993');
    expect(within(lotr).getByRole('slider', { name: `Your rating for ${LOTR}` })).toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 2, name: 'Forrest Gump' })).toBeInTheDocument();
  });

  it('shows progress towards 10 ratings, counting the ratings the user already has', async () => {
    await openPage(); // listRatings fixture: 5 ratings
    expect(progress()).toHaveAttribute('value', '5');
    expect(progress()).toHaveAttribute('max', '10');
    expect(screen.queryByRole('link', { name: 'Continue' })).not.toBeInTheDocument();
    expect(screen.getByText(/Rate 5 more/)).toBeInTheDocument();
  });

  it('rates optimistically, sends the rating and advances the progress bar at once', async () => {
    const puts = recordRequests('/api/ratings/4993', 'PUT');
    await openPage();
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4.5);
    expect(slider).toHaveAttribute('aria-valuetext', '4.5 stars');
    expect(screen.getByText('6 of 10 rated')).toBeInTheDocument();
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(await puts[0]!.json()).toEqual({ rating: 4.5 });
  });

  it('rolls back the rating and the progress when the save fails', async () => {
    server.use(handlerFor('putRating', 'not_found'));
    await openPage();
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 3);
    expect(screen.getByText('6 of 10 rated')).toBeInTheDocument();
    expect(await within(card(LOTR)).findByRole('alert')).toHaveTextContent("Couldn't save your rating.");
    expect(slider).toHaveAttribute('aria-valuetext', 'Not rated');
    expect(screen.getByText('5 of 10 rated')).toBeInTheDocument();
  });

  it('offers "Continue" to For you once there are 10 ratings', async () => {
    const user = userEvent.setup();
    serveRatings(9);
    const { router } = renderApp('/onboarding');
    await screen.findByText('9 of 10 rated');
    expect(screen.queryByRole('link', { name: 'Continue' })).not.toBeInTheDocument();
    rateWithKeyboard(within(await findCard(LOTR)).getByRole('slider'), 4);
    const next = await screen.findByRole('link', { name: 'Continue' });
    expect(next).toHaveAttribute('href', '/');
    await user.click(next);
    expect(router.state.location.pathname).toBe('/');
  });

  it('clearing a rating counts it back down', async () => {
    serveRatings(9);
    renderApp('/onboarding');
    await screen.findByText('9 of 10 rated');
    const slider = within(await findCard(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4);
    await screen.findByText('10 of 10 rated');
    await userEvent.setup().click(screen.getByRole('button', { name: `Clear Your rating for ${LOTR}` }));
    expect(await screen.findByText('9 of 10 rated')).toBeInTheDocument();
  });

  it('searches as you type (debounced 300 ms, one request) and lets you rate a result', async () => {
    const searches = recordRequests('/api/movies/search');
    await openPage();
    const user = fakeClock();
    await user.type(screen.getByRole('searchbox', { name: 'Search for any movie' }), 'star');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(299);
    });
    expect(searches).toHaveLength(0);
    expect(screen.queryByRole('region', { name: 'Search results' })).not.toBeInTheDocument();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1);
    });
    const results = await screen.findByRole('region', { name: 'Search results' });
    await within(results).findByRole('heading', { name: 'Star Wars: Episode V - The Empire Strikes Back' });
    expect(searches).toHaveLength(1);
    expect(queryOf(searches[0]!).q).toBe('star');
    // Star Wars IV is already rated 5 in the listRatings fixture; the control shows it.
    expect(
      within(results).getByRole('slider', { name: 'Your rating for Star Wars: Episode IV - A New Hope' }),
    ).toHaveAttribute('aria-valuetext', '5 stars');
    rateWithKeyboard(
      within(results).getByRole('slider', { name: 'Your rating for Star Wars: Episode VI - Return of the Jedi' }),
      4,
    );
    expect(screen.getByText('6 of 10 rated')).toBeInTheDocument();
  });

  it('says when a search finds nothing, and when it fails', async () => {
    server.use(http.get('/api/movies/search', () => HttpResponse.json({ items: [] })));
    await openPage();
    const user = fakeClock();
    const box = screen.getByRole('searchbox', { name: 'Search for any movie' });
    await user.type(box, 'zzzz');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(300);
    });
    expect(await screen.findByText('No movies match “zzzz”.')).toBeInTheDocument();
    server.use(handlerFor('searchMovies', 'model_unavailable'));
    await user.type(box, 'q');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(300);
    });
    await waitFor(() =>
      expect(within(screen.getByRole('region', { name: 'Search results' })).getByRole('alert')).toHaveTextContent(
        'Recommendations are temporarily unavailable',
      ),
    );
  });

  it('shows an error with a retry when the movie list cannot load, and search keeps its own state', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('getOnboarding', 'model_unavailable'));
    renderApp('/onboarding');
    const main = screen.getByRole('main');
    const alert = await within(main).findByRole('alert');
    expect(alert).toHaveTextContent('Recommendations are temporarily unavailable');
    expect(screen.getByRole('searchbox', { name: 'Search for any movie' })).toBeInTheDocument();
    server.use(handlerFor('getOnboarding', 'ok'));
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('heading', { level: 2, name: LOTR })).toBeInTheDocument();
  });

  it('shows an empty state when there is nothing left to suggest', async () => {
    server.use(http.get('/api/onboarding', () => HttpResponse.json({ items: [] })));
    renderApp('/onboarding');
    expect(await screen.findByText(/rated everything we'd suggest/)).toBeInTheDocument();
  });

  it('falls back to the account rating count when the ratings list fails', async () => {
    server.use(http.get('/api/ratings', () => errorBody('internal', 500)));
    renderApp('/onboarding');
    expect(await screen.findByText('5 of 10 rated')).toBeInTheDocument(); // getMe: rating_count 5
  });

  it('asks a signed-out visitor to log in', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderApp('/onboarding');
    expect(await screen.findByRole('link', { name: /Log in to rate movies/ })).toBeInTheDocument();
  });

  it('has no axe violations (loaded, searching)', async () => {
    await openPage();
    expect(await axeViolations()).toEqual([]);
    const user = fakeClock();
    await user.type(screen.getByRole('searchbox', { name: 'Search for any movie' }), 'star');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(300);
    });
    await screen.findByRole('heading', { name: 'Star Wars: Episode V - The Empire Strikes Back' });
    realClock();
    expect(await axeViolations()).toEqual([]);
  });

  it('has no axe violations when the list fails to load', async () => {
    server.use(handlerFor('getOnboarding', 'model_unavailable'));
    renderApp('/onboarding');
    await within(screen.getByRole('main')).findByRole('alert');
    await act(async () => {});
    expect(await axeViolations()).toEqual([]);
  });
});

async function findCard(title: string): Promise<HTMLElement> {
  await screen.findByRole('heading', { name: title });
  return card(title);
}
