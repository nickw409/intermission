import { act, cleanup, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
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
    localStorage.clear();
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

  it('"Not interested" hides the card and saves it through the feedback endpoint', async () => {
    const user = userEvent.setup();
    const spy = vi.spyOn(feedbackHook, 'notInterested');
    await openPage();
    const sent = recordRequests('/api/feedback', 'POST');
    await user.click(within(card(LOTR)).getByRole('button', { name: `Not interested in ${LOTR}` }));
    expect(screen.queryByRole('heading', { level: 2, name: LOTR })).not.toBeInTheDocument();
    expect(screen.getAllByRole('article')).toHaveLength(2);
    expect(spy).toHaveBeenCalledWith({ movieId: 4993, model: 'als' });
    await waitFor(() => expect(sent).toHaveLength(1));
    expect(await sent[0]!.json()).toEqual({ movieId: 4993, model: 'als', action: 'not_interested' });
    expect(sent[0]!.headers.get('X-Requested-With')).toBe('recs');
    // Focus does not get lost when the card disappears.
    expect(document.activeElement).not.toBe(document.body);
    // Saved: the card stays hidden and no error appears.
    await waitFor(() => expect(spy).toHaveBeenCalledTimes(1));
    expect(screen.queryByRole('heading', { level: 2, name: LOTR })).not.toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    spy.mockRestore();
  });

  it('brings the card back with an error when "Not interested" cannot be saved', async () => {
    const user = userEvent.setup();
    server.use(http.post('/api/feedback', () => errorBody('internal', 500)));
    await openPage();
    await user.click(within(card(LOTR)).getByRole('button', { name: `Not interested in ${LOTR}` }));
    const back = await screen.findByRole('heading', { level: 2, name: LOTR });
    expect(within(back.closest('article')!).getByRole('alert')).toHaveTextContent(/Couldn't save "Not interested"/);
    expect(screen.getAllByRole('article')).toHaveLength(3);

    // A retry that works hides it again and clears the error.
    server.resetHandlers();
    await user.click(within(card(LOTR)).getByRole('button', { name: `Not interested in ${LOTR}` }));
    expect(screen.queryByRole('heading', { level: 2, name: LOTR })).not.toBeInTheDocument();
    await waitFor(() => expect(screen.getAllByRole('article')).toHaveLength(2));
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
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

const MODEL_OPTIONS = [
  'Popularity',
  'Popularity (count)',
  'Item-KNN',
  'Content-based',
  'Matrix factorization',
  'Implicit ALS (default)',
];
const STORAGE_KEY = 'intermission.for-you';

/** Answers /api/recommend with `count` movies, echoing the requested model. */
function recommendWith(count: number) {
  return http.get('/api/recommend', ({ request }) =>
    HttpResponse.json({
      model: new URL(request.url).searchParams.get('model') ?? 'als',
      model_version: 'v',
      cold_start: false,
      items: Array.from({ length: count }, (_, i) => ({
        movieId: 1000 + i,
        title: `Movie ${i + 1}`,
        year: 2000,
        score: 1 - i / 100,
        because: [],
      })),
    }),
  );
}

function modelSelect(): HTMLSelectElement {
  return screen.getByRole('combobox', { name: 'Model' });
}

function chip(name: string): HTMLElement {
  return within(screen.getByRole('group', { name: 'Categories' })).getByRole('button', { name });
}

function stored(): unknown {
  const raw = localStorage.getItem(STORAGE_KEY);
  return raw === null ? null : JSON.parse(raw);
}

describe('For you: model selector and categories', () => {
  beforeEach(() => localStorage.clear());
  afterEach(() => {
    localStorage.clear();
    vi.restoreAllMocks();
  });

  it('lists every loaded model by name, with the default preselected and marked', async () => {
    await openPage();
    const select = modelSelect();
    expect(within(select).getAllByRole('option').map((o) => o.textContent)).toEqual(MODEL_OPTIONS);
    expect(select.value).toBe('als');
    expect(screen.getByRole('main')).toHaveTextContent('Picks from Implicit ALS');
  });

  it('refetches with the chosen model, shows it in the header and keeps it in the URL and storage', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(20));
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    const recommend = recordRequests('/api/recommend');
    await user.selectOptions(modelSelect(), 'knn');
    await screen.findByText('Picks from Item-KNN');
    expect(recommend.map(queryOf)).toEqual([{ model: 'knn', k: '20' }]);
    expect(new URLSearchParams(router.state.location.search).get('model')).toBe('knn');
    expect(stored()).toEqual({ model: 'knn', genres: [] });
  });

  it('shows the genres as toggle buttons, IMAX last, none pressed at first', async () => {
    await openPage();
    const group = screen.getByRole('group', { name: 'Categories' });
    const chips = within(group)
      .getAllByRole('button')
      .filter((b) => b.hasAttribute('aria-pressed'));
    expect(chips).toHaveLength(19);
    expect(chips[0]).toHaveAccessibleName('Action');
    expect(chips.at(-1)).toHaveAccessibleName('IMAX');
    for (const c of chips) expect(c).toHaveAttribute('aria-pressed', 'false');
    expect(within(group).getByRole('button', { name: 'Clear categories' })).toBeDisabled();
    expect(group.tagName).toBe('FIELDSET');
  });

  it('debounces rapid chip toggles into one request, while the buttons and URL update at once', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(20));
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    const recommend = recordRequests('/api/recommend');
    await user.click(chip('Horror'));
    await user.click(chip('Action'));
    await user.click(chip('Comedy'));
    await user.click(chip('Action'));
    expect(chip('Horror')).toHaveAttribute('aria-pressed', 'true');
    expect(chip('Action')).toHaveAttribute('aria-pressed', 'false');
    expect(new URLSearchParams(router.state.location.search).get('genres')).toBe('Comedy,Horror');
    expect(recommend).toHaveLength(0);
    await waitFor(() => expect(recommend).toHaveLength(1));
    await act(async () => {
      await new Promise((r) => setTimeout(r, 700));
    });
    expect(recommend.map(queryOf)).toEqual([{ model: 'als', k: '20', genres: 'Comedy,Horror' }]);
  });

  it('remembers genres but not a model the user never picked', async () => {
    const user = userEvent.setup();
    await openPage();
    await user.click(chip('Horror'));
    expect(stored()).toEqual({ genres: ['Horror'] });
  });

  it('forgets the remembered model when the user picks the server default', async () => {
    const user = userEvent.setup();
    await openPage();
    await user.selectOptions(modelSelect(), 'knn');
    expect(stored()).toEqual({ model: 'knn', genres: [] });
    await user.selectOptions(modelSelect(), 'als');
    expect(stored()).toEqual({ genres: [] });
  });

  it('never remembers or links the guessed model when the model list is down and the page fell back to pop', async () => {
    const user = userEvent.setup();
    server.use(
      http.get('/api/models', () => errorBody('internal', 500)),
      http.get('/api/recommend', ({ request }) =>
        new URL(request.url).searchParams.get('model') === 'pop'
          ? HttpResponse.json(handlerBody('recommend', 'ok'))
          : errorBody('unknown_model', 400),
      ),
    );
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 2, name: LOTR });
    expect(router.state.location.search).toBe('');
    await user.click(chip('Fantasy'));
    const params = new URLSearchParams(router.state.location.search);
    expect(params.has('model')).toBe(false);
    expect(params.get('genres')).toBe('Fantasy');
    expect(stored()).toEqual({ genres: ['Fantasy'] });
  });

  it('announces the result count politely after a change', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(20));
    renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    await user.click(chip('Horror'));
    const message = await screen.findByText('20 horror picks from Implicit ALS');
    expect(message.closest('[aria-live="polite"]')).not.toBeNull();
    await user.selectOptions(modelSelect(), 'knn');
    expect(await screen.findByText('20 horror picks from Item-KNN')).toBe(message);
  });

  it('sends the chosen genres comma-joined in enum order, and Clear removes them', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(20));
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    const recommend = recordRequests('/api/recommend');

    await user.click(chip('Horror'));
    expect(chip('Horror')).toHaveAttribute('aria-pressed', 'true');
    await waitFor(() => expect(recommend).toHaveLength(1));
    await user.click(chip('Action'));
    await waitFor(() => expect(recommend).toHaveLength(2));
    expect(queryOf(recommend[0]!)).toEqual({ model: 'als', k: '20', genres: 'Horror' });
    expect(queryOf(recommend[1]!)).toEqual({ model: 'als', k: '20', genres: 'Action,Horror' });
    expect(new URLSearchParams(router.state.location.search).get('genres')).toBe('Action,Horror');
    expect(stored()).toEqual({ genres: ['Action', 'Horror'] });
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });

    await user.click(chip('Clear categories'));
    await waitFor(() => expect(recommend).toHaveLength(3));
    expect(queryOf(recommend[2]!)).toEqual({ model: 'als', k: '20' });
    expect(chip('Horror')).toHaveAttribute('aria-pressed', 'false');
    expect(new URLSearchParams(router.state.location.search).has('genres')).toBe(false);
    expect(document.activeElement).not.toBe(document.body);
  });

  it('chips work from the keyboard', async () => {
    const user = userEvent.setup();
    await openPage();
    const recommend = recordRequests('/api/recommend');
    act(() => chip('Comedy').focus());
    await user.keyboard('{Enter}');
    expect(chip('Comedy')).toHaveAttribute('aria-pressed', 'true');
    await waitFor(() => expect(recommend).toHaveLength(1));
    expect(queryOf(recommend[0]!).genres).toBe('Comedy');
  });

  it('starts from the model and genres in the URL', async () => {
    server.use(recommendWith(20));
    const recommend = recordRequests('/api/recommend');
    renderApp('/?model=knn&genres=Horror,Action');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(recommend.map(queryOf)).toEqual([{ model: 'knn', k: '20', genres: 'Action,Horror' }]);
    expect(modelSelect().value).toBe('knn');
    expect(chip('Horror')).toHaveAttribute('aria-pressed', 'true');
    expect(chip('Action')).toHaveAttribute('aria-pressed', 'true');
    expect(chip('Drama')).toHaveAttribute('aria-pressed', 'false');
  });

  it.each([
    ['an unknown model and genre', '/?model=bogus&genres=Horror,Nope', { model: 'als', k: '20', genres: 'Horror' }],
    ['a model that is not loaded', '/?model=hybrid', { model: 'als', k: '20' }],
    ['only unknown genres', '/?genres=horror,,Sci%20Fi', { model: 'als', k: '20' }],
  ])('ignores %s in the URL', async (_, path, expected) => {
    server.use(recommendWith(20));
    const recommend = recordRequests('/api/recommend');
    renderApp(path);
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(recommend.map(queryOf)).toEqual([expected]);
  });

  it('remembers the last choice in this browser when the URL has none', async () => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ model: 'content', genres: ['Comedy', 'Animation'] }));
    server.use(recommendWith(20));
    const recommend = recordRequests('/api/recommend');
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(recommend.map(queryOf)).toEqual([{ model: 'content', k: '20', genres: 'Animation,Comedy' }]);
    expect(modelSelect().value).toBe('content');
    // Remembered values are never written into the address on their own.
    expect(router.state.location.search).toBe('');
  });

  it.each([
    ['model and genres', '/?model=mf&genres=War', { model: 'mf', k: '20', genres: 'War' }],
    ['only a model', '/?model=mf', { model: 'mf', k: '20' }],
    ['only genres', '/?genres=War', { model: 'als', k: '20', genres: 'War' }],
    ['an empty genres key', '/?genres=', { model: 'als', k: '20' }],
  ])('a URL with %s is the whole view, ignoring the remembered choice', async (_, path, expected) => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ model: 'content', genres: ['Comedy'] }));
    const recommend = recordRequests('/api/recommend');
    const { router } = renderApp(path);
    await screen.findByRole('heading', { level: 2, name: LOTR });
    expect(recommend.map(queryOf)).toEqual([expected]);
    expect(router.state.location.search).toBe(path.slice(1));
  });

  it.each([
    ['unknown values', JSON.stringify({ model: 'bogus', genres: ['Nope', 5, 'horror'] })],
    ['a model that is not loaded', JSON.stringify({ model: 'mmr', genres: 'Horror' })],
    ['malformed JSON', '{not json'],
    ['the wrong shape', JSON.stringify(['knn', 'Horror'])],
  ])('ignores a remembered choice with %s', async (_, raw) => {
    localStorage.setItem(STORAGE_KEY, raw);
    const recommend = recordRequests('/api/recommend');
    await openPage();
    expect(recommend.map(queryOf)).toEqual([{ model: 'als', k: '20' }]);
  });

  it('still works when browser storage is unavailable', async () => {
    const user = userEvent.setup();
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new DOMException('denied', 'SecurityError');
    });
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new DOMException('denied', 'SecurityError');
    });
    const recommend = recordRequests('/api/recommend');
    await openPage();
    await user.selectOptions(modelSelect(), 'knn');
    await waitFor(() => expect(recommend).toHaveLength(2));
    expect(queryOf(recommend[1]!)).toEqual({ model: 'knn', k: '20' });
  });

  it('says when a category filter returns fewer than 20 movies', async () => {
    server.use(recommendWith(3));
    renderApp('/?genres=Horror');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(screen.getByRole('main')).toHaveTextContent(
      'Only 3 horror picks right now — rate more movies or widen the categories.',
    );
  });

  it('counts only the cards still shown in the short-list message and uses the same words in the summary', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(3));
    renderApp('/?genres=Horror,Action');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    const main = screen.getByRole('main');
    expect(main).toHaveTextContent('Picks from Implicit ALS · action or horror');
    await user.click(within(card('Movie 1')).getByRole('button', { name: 'Not interested in Movie 1' }));
    expect(main).toHaveTextContent('Only 2 action or horror picks right now');
  });

  it('names several categories in the short-list message', async () => {
    server.use(recommendWith(1));
    renderApp('/?genres=Horror,Sci-Fi,Action');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(screen.getByRole('main')).toHaveTextContent(
      'Only 1 action, horror or sci-fi pick right now — rate more movies or widen the categories.',
    );
  });

  it('does not show the short-list message for a full list or without categories', async () => {
    server.use(recommendWith(20));
    renderApp('/?genres=Horror');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(screen.queryByText(/Only \d+/)).not.toBeInTheDocument();
    cleanup();
    server.use(recommendWith(3));
    renderApp('/');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(screen.queryByText(/Only \d+/)).not.toBeInTheDocument();
  });

  it('shows an empty state with "Clear categories" when the filter matches nothing', async () => {
    const user = userEvent.setup();
    server.use(recommendWith(0));
    renderApp('/?genres=Western');
    const main = screen.getByRole('main');
    const empty = (await within(main).findByText('No western picks right now.')).parentElement!;
    expect(await axeViolations()).toEqual([]);
    const recommend = recordRequests('/api/recommend');
    server.use(recommendWith(20));
    await user.click(within(empty).getByRole('button', { name: 'Clear categories' }));
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(recommend.map(queryOf)).toEqual([{ model: 'als', k: '20' }]);
    expect(chip('Western')).toHaveAttribute('aria-pressed', 'false');
  });

  it('keeps the cold-start message with categories chosen', async () => {
    server.use(handlerFor('recommend', 'ok_cold_start'));
    const recommend = recordRequests('/api/recommend');
    renderApp('/?genres=Drama');
    await screen.findByRole('heading', { level: 2, name: 'The Shawshank Redemption' });
    expect(screen.getByRole('main')).toHaveTextContent(/Rate at least 5 movies/);
    expect(queryOf(recommend[0]!).genres).toBe('Drama');
  });

  it('refetches with the same model and genres after a rating change', async () => {
    const recommend = recordRequests('/api/recommend');
    renderApp('/?model=knn&genres=Fantasy');
    await screen.findByRole('heading', { level: 2, name: LOTR });
    rateWithKeyboard(within(card(LOTR)).getByRole('slider'), 4);
    await waitFor(() => expect(recommend).toHaveLength(2));
    expect(queryOf(recommend[1]!)).toEqual({ model: 'knn', k: '20', genres: 'Fantasy' });
  });

  it('hides the model selector when the model list is unavailable, still showing categories', async () => {
    server.use(http.get('/api/models', () => errorBody('internal', 500)));
    await openPage();
    expect(screen.queryByRole('combobox', { name: 'Model' })).not.toBeInTheDocument();
    expect(chip('Horror')).toBeInTheDocument();
  });

  it('has no axe violations with categories chosen', async () => {
    server.use(recommendWith(3));
    renderApp('/?model=knn&genres=Horror,Action');
    await screen.findByRole('heading', { level: 2, name: 'Movie 1' });
    expect(await axeViolations()).toEqual([]);
  });
});

function handlerBody(op: 'recommend', name: string): Record<string, unknown> {
  return fixtureFor(op).cases.find((c) => c.name === name)!.response.body as Record<string, unknown>;
}
