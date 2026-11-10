import { act, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { feedbackHook } from '../../lib/feedback';
import { errorBody, gate, queryOf, rateWithKeyboard, recordRequests } from '../../lib/test-helpers';
import { fixtureFor, handlerFor, responseFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const BANNER = 'Recommendations are temporarily unavailable';
const LOTR = 'The Lord of the Rings: The Fellowship of the Ring';

async function openPage() {
  const app = renderApp('/');
  await screen.findByRole('heading', { level: 2, name: LOTR });
  return app;
}

/** The shell's live region that holds the model-unavailable banner. */
function bannerRegion(): HTMLElement {
  const region = screen.getAllByRole('status').find((el) => el.closest('main') === null);
  if (!region) throw new Error('no banner region');
  return region;
}

function card(title: string): HTMLElement {
  const heading = screen.getByRole('heading', { level: 2, name: title });
  const article = heading.closest('article');
  if (!article) throw new Error(`no card for ${title}`);
  return article;
}

describe('For you page', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('shows a loading state, then the top 20 from the default model', async () => {
    const recommend = recordRequests('/api/recommend');
    renderApp('/');
    expect(screen.getByRole('heading', { level: 1, name: 'For you' })).toBeInTheDocument();
    expect(await screen.findByText(/Loading your recommendations/)).toBeInTheDocument();
    await screen.findByRole('heading', { level: 2, name: LOTR });
    expect(recommend).toHaveLength(1);
    expect(queryOf(recommend[0]!)).toEqual({ model: 'als', k: '20' }); // listModels fixture: default_model als
    expect(screen.getAllByRole('article')).toHaveLength(3);
  });

  it('shows poster cards with alt text, a link to the movie and the explanation', async () => {
    await openPage();
    const lotr = card(LOTR);
    const poster = within(lotr).getByRole('img', { name: `${LOTR} (2001)` });
    expect(poster).toHaveAttribute('src', '/api/posters/4993');
    expect(within(lotr).getByRole('link', { name: LOTR })).toHaveAttribute('href', '/movies/4993');
    expect(lotr).toHaveTextContent('Because you rated Star Wars: Episode IV - A New Hope and The Matrix');
    expect(within(lotr).getByRole('slider', { name: `Your rating for ${LOTR}` })).toHaveAttribute(
      'aria-valuetext',
      'Not rated',
    );
  });

  it('falls back to knn when the default model is not loaded', async () => {
    server.use(
      http.get('/api/models', () =>
        HttpResponse.json({
          artifact_version: 'v',
          trained_at: '2026-11-09T21:14:03Z',
          default_model: 'als',
          models: ['pop', 'knn'],
        }),
      ),
    );
    const recommend = recordRequests('/api/recommend');
    await openPage();
    expect(queryOf(recommend[0]!).model).toBe('knn');
  });

  it('falls back to knn, then pop, when the model list is unavailable', async () => {
    server.use(
      http.get('/api/models', () => errorBody('internal', 500)),
      http.get('/api/recommend', ({ request }) =>
        new URL(request.url).searchParams.get('model') === 'pop'
          ? HttpResponse.json(handlerBody('recommend', 'ok'))
          : errorBody('unknown_model', 400),
      ),
    );
    const recommend = recordRequests('/api/recommend');
    await openPage();
    expect(recommend.map((r) => queryOf(r).model)).toEqual(['knn', 'pop']);
  });

  it('shows the popularity list with a prompt to rate more when cold_start is true', async () => {
    server.use(handlerFor('recommend', 'ok_cold_start'));
    renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'The Shawshank Redemption' });
    const main = screen.getByRole('main');
    expect(main).toHaveTextContent(/Rate at least 5 movies/);
    expect(within(main).getByRole('link', { name: /Rate more movies/ })).toHaveAttribute('href', '/onboarding');
  });

  it('shows an empty state when there is nothing to recommend', async () => {
    server.use(
      http.get('/api/recommend', () =>
        HttpResponse.json({ model: 'als', model_version: 'v', items: [], cold_start: false }),
      ),
    );
    renderApp('/');
    expect(await screen.findByText(/No recommendations right now/)).toBeInTheDocument();
  });

  it('"Not interested" hides the card locally and goes through the feedback hook, sending nothing yet', async () => {
    const user = userEvent.setup();
    const spy = vi.spyOn(feedbackHook, 'notInterested');
    await openPage();
    const sent = recordRequests('/api/feedback');
    await user.click(within(card(LOTR)).getByRole('button', { name: `Not interested in ${LOTR}` }));
    expect(screen.queryByRole('heading', { level: 2, name: LOTR })).not.toBeInTheDocument();
    expect(screen.getAllByRole('article')).toHaveLength(2);
    expect(spy).toHaveBeenCalledWith({ movieId: 4993, model: 'als' });
    expect(sent).toHaveLength(0);
    // Focus does not get lost when the card disappears.
    expect(document.activeElement).not.toBe(document.body);
    spy.mockRestore();
  });

  it('updates a rating optimistically and sends it', async () => {
    const puts = recordRequests('/api/ratings/4993', 'PUT');
    const { wait, release } = gate();
    server.use(
      http.put('/api/ratings/:movieId', async () => {
        await wait;
        return HttpResponse.json({ movieId: 4993, title: 'x', year: 2001, rating: 4, updated_at: 1 });
      }),
    );
    await openPage();
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4);
    expect(slider).toHaveAttribute('aria-valuetext', '4 stars'); // before the server answers
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(await puts[0]!.json()).toEqual({ rating: 4 });
    release();
    await waitFor(() => expect(slider).toHaveAttribute('aria-valuetext', '4 stars'));
  });

  it('rolls a rating back and says so when saving fails', async () => {
    server.use(handlerFor('putRating', 'too_many_ratings'));
    await openPage();
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4);
    expect(slider).toHaveAttribute('aria-valuetext', '4 stars');
    expect(await within(card(LOTR)).findByRole('alert')).toHaveTextContent(
      "Couldn't save your rating. You've reached the maximum number of ratings.",
    );
    expect(slider).toHaveAttribute('aria-valuetext', 'Not rated');
  });

  it('refetches recommendations once, 500 ms after rating changes settle, keeping the cards up meanwhile', async () => {
    await openPage();
    const recommend = recordRequests('/api/recommend');
    // Hold the refetch open so the page can be checked while it is in flight.
    const refetch = gate();
    server.use(
      http.get('/api/recommend', async () => {
        await refetch.wait;
        return responseFor(fixtureFor('recommend').cases[0]!);
      }),
    );
    const flush = async (until: () => boolean, what: string) => {
      for (let i = 0; i < 50 && !until(); i++) {
        await act(async () => {
          await vi.advanceTimersByTimeAsync(0);
        });
      }
      if (!until()) throw new Error(`timed out waiting for ${what}`);
    };
    const puts = recordRequests(/^\/api\/ratings\//, 'PUT');
    let saved = 0;
    server.events.on('response:mocked', ({ request }) => {
      if (request.method === 'PUT') saved += 1;
    });

    vi.useFakeTimers();
    rateWithKeyboard(within(card(LOTR)).getByRole('slider'), 4);
    rateWithKeyboard(within(card('Fight Club')).getByRole('slider'), 3);
    // Let both PUTs finish without moving the clock.
    await flush(() => saved >= 2, `both rating PUTs to finish (finished: ${saved})`);
    expect(puts).toHaveLength(2);

    await act(async () => {
      await vi.advanceTimersByTimeAsync(499);
    });
    expect(recommend).toHaveLength(0);
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1);
    });
    await flush(() => recommend.length > 0, 'the refetch to start');
    expect(recommend).toHaveLength(1);
    // A quiet refresh: the cards stay on screen, marked busy, while the request is in flight.
    const list = screen.getByRole('list', { name: 'Recommendations' });
    expect(list).toHaveAttribute('aria-busy', 'true');
    expect(screen.getAllByRole('article')).toHaveLength(3);
    expect(screen.queryByText(/Loading your recommendations/)).not.toBeInTheDocument();
    refetch.release();
    await flush(() => !list.hasAttribute('aria-busy'), 'the refetch to finish');
    expect(screen.getAllByRole('article')).toHaveLength(3);
    await act(async () => {
      await vi.advanceTimersByTimeAsync(5000);
    });
    expect(recommend).toHaveLength(1);
    expect(queryOf(recommend[0]!)).toEqual({ model: 'als', k: '20' });
  });

  it('does not refetch after a rating that failed to save', async () => {
    server.use(handlerFor('putRating', 'not_found'));
    await openPage();
    const recommend = recordRequests('/api/recommend');
    const slider = within(card(LOTR)).getByRole('slider');
    rateWithKeyboard(slider, 4);
    await within(card(LOTR)).findByRole('alert');
    await act(async () => {
      await new Promise((r) => setTimeout(r, 700));
    });
    expect(recommend).toHaveLength(0);
  });

  it('shows the banner and an error with a retry on 503 model_unavailable', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('recommend', 'model_unavailable'));
    renderApp('/');
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent('Recommendations are temporarily unavailable');
    await waitFor(() => expect(bannerRegion()).toHaveTextContent(BANNER));
    server.use(handlerFor('recommend', 'ok'));
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('heading', { level: 2, name: LOTR })).toBeInTheDocument();
  });

  it('shows a placeholder poster for a malformed movie id instead of failing the page', async () => {
    server.use(
      http.get('/api/recommend', () =>
        HttpResponse.json({
          model: 'als',
          model_version: 'v',
          cold_start: false,
          items: [
            { movieId: 0, title: 'Broken', year: 2000, score: 1, because: [] },
            { movieId: 2959, title: 'Fight Club', year: 1999, score: 0.9, because: [] },
          ],
        }),
      ),
    );
    renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Fight Club' });
    const broken = within(card('Broken')).getByRole('img', { name: 'Broken (2000)' });
    expect(broken.tagName).not.toBe('IMG');
    expect(within(card('Fight Club')).getByRole('img', { name: 'Fight Club (1999)' })).toHaveAttribute(
      'src',
      '/api/posters/2959',
    );
  });

  it('asks a signed-out visitor to log in', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    const recommend = recordRequests('/api/recommend');
    renderApp('/');
    const main = screen.getByRole('main');
    expect(await within(main).findByRole('link', { name: /Log in to see/ })).toHaveAttribute('href', '/login');
    expect(recommend).toHaveLength(0);
  });

  it('has no axe violations when loaded and after rating a movie', async () => {
    await openPage();
    rateWithKeyboard(within(card(LOTR)).getByRole('slider'), 4);
    expect(await axeViolations()).toEqual([]);
  });

  it.each([
    ['cold start', () => server.use(handlerFor('recommend', 'ok_cold_start')), /Rate at least 5/],
    ['model down', () => server.use(handlerFor('recommend', 'model_unavailable')), /Try again/],
    ['signed out', () => server.use(handlerFor('getMe', 'unauthorized')), /Log in to see/],
  ])('has no axe violations: %s', async (_, setup, marker) => {
    setup();
    renderApp('/');
    await screen.findAllByText(marker);
    expect(await axeViolations()).toEqual([]);
  });
});

function handlerBody(op: 'recommend', name: string): Record<string, unknown> {
  return fixtureFor(op).cases.find((c) => c.name === name)!.response.body as Record<string, unknown>;
}
