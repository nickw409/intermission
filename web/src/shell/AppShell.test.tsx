import { act, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { api } from '../api/client';
import { handlerFor } from '../mocks/handlers';
import { axeViolations } from '../test/axe';
import { renderApp } from '../test/render';
import { server } from '../test/setup';

function countRequests(method: string, pathname: string) {
  const seen: Request[] = [];
  server.events.on('request:start', ({ request }) => {
    if (request.method === method && new URL(request.url).pathname === pathname) seen.push(request);
  });
  return seen;
}

const GROUPLENS =
  'Data from MovieLens by GroupLens Research; not endorsed by the University of Minnesota or GroupLens';
const TMDB = 'This product uses the TMDB API but is not endorsed or certified by TMDB';
const BANNER = 'Recommendations are temporarily unavailable';

describe('layout', () => {
  it('has the main navigation and an account menu for the signed-in user', async () => {
    renderApp('/');
    const nav = screen.getByRole('navigation', { name: 'Main' });
    const links = within(nav).getAllByRole('link').map((a) => a.textContent);
    expect(links).toEqual(['For you', 'Compare', 'My ratings', 'Evaluation', 'How it works']);
    const account = await screen.findByRole('button', { name: /alice/ });
    expect(account).toHaveAttribute('aria-expanded', 'false');
  });

  it('marks the current page in the navigation', async () => {
    renderApp('/compare');
    const nav = screen.getByRole('navigation', { name: 'Main' });
    expect(within(nav).getByRole('link', { name: 'Compare' })).toHaveAttribute('aria-current', 'page');
    expect(within(nav).getByRole('link', { name: 'For you' })).not.toHaveAttribute('aria-current');
    await screen.findByRole('button', { name: /alice/ });
  });

  it('has a footer with the GroupLens and TMDB attributions', async () => {
    renderApp('/');
    const footer = screen.getByRole('contentinfo');
    expect(footer).toHaveTextContent(GROUPLENS);
    expect(footer).toHaveTextContent(TMDB);
    await screen.findByRole('button', { name: /alice/ });
  });

  it('offers no dataset or model downloads', async () => {
    const { container } = renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    expect(container.querySelector('a[download]')).toBeNull();
    for (const a of container.querySelectorAll('a[href]')) {
      expect(a.getAttribute('href')).not.toMatch(/\.(csv|zip|gz|tgz|tar|bin|npy|npz|json|parquet)(\?|$)/i);
    }
  });

  it('offers a skip link to the main content', async () => {
    renderApp('/');
    const skip = screen.getByRole('link', { name: 'Skip to content' });
    expect(skip).toHaveAttribute('href', '#main');
    expect(screen.getByRole('main')).toHaveAttribute('id', 'main');
    await screen.findByRole('button', { name: /alice/ });
  });

  it('has no axe violations when signed in with the account menu open', async () => {
    renderApp('/');
    await userEvent.setup().click(await screen.findByRole('button', { name: /alice/ }));
    expect(await axeViolations()).toEqual([]);
  });

  it('has no axe violations when signed out', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderApp('/');
    await screen.findByRole('link', { name: 'Log in' });
    expect(await axeViolations()).toEqual([]);
  });
});

describe('auth state', () => {
  it('asks GET /api/me once on load', async () => {
    const seen = countRequests('GET', '/api/me');
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    expect(seen).toHaveLength(1);
  });

  it('shows the logged-out navigation when /api/me returns 401', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderApp('/');
    expect(await screen.findByRole('link', { name: 'Log in' })).toHaveAttribute('href', '/login');
    expect(screen.getByRole('link', { name: 'Sign up' })).toHaveAttribute('href', '/signup');
    expect(screen.queryByRole('button', { name: /alice/ })).not.toBeInTheDocument();
  });

  it.each([
    ['a 500', () => HttpResponse.json({ error: { code: 'internal', message: 'boom' } }, { status: 500 })],
    ['a network failure', () => HttpResponse.error()],
  ])('does not claim "logged out" after %s from /api/me, and offers a retry', async (_, resolver) => {
    const user = userEvent.setup();
    server.use(http.get('/api/me', resolver, { once: true }));
    renderApp('/');
    expect(await screen.findByText("Couldn't check your sign-in.")).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Log in' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Retry' }));
    expect(await screen.findByRole('button', { name: /alice/ })).toBeInTheDocument();
  });

  it('a late /api/me 401 does not undo a login that finished first', async () => {
    const user = userEvent.setup();
    let release: () => void = () => {};
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    server.use(
      http.get('/api/me', async () => {
        await gate;
        return HttpResponse.json({ error: { code: 'unauthorized', message: 'not logged in' } }, { status: 401 });
      }),
    );
    const { router } = renderApp('/login');
    await user.type(screen.getByLabelText('Username'), 'alice');
    await user.type(screen.getByLabelText('Password'), 'correct horse battery');
    await user.click(within(screen.getByRole('main')).getByRole('button', { name: 'Log in' }));
    expect(await screen.findByRole('button', { name: /alice/ })).toBeInTheDocument();
    await act(async () => {
      release();
      await new Promise((r) => setTimeout(r, 20));
    });
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Log in' })).not.toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/onboarding');
  });

  it('switches to logged out when any call reports the session has expired', async () => {
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    server.use(handlerFor('listRatings', 'unauthorized'));
    await act(async () => {
      await api.get('/api/ratings').catch(() => undefined);
    });
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /alice/ })).not.toBeInTheDocument();
  });

  it('ignores a 401 from a request that was sent before the user logged in', async () => {
    const user = userEvent.setup();
    let release: () => void = () => {};
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    server.use(
      handlerFor('getMe', 'unauthorized'),
      http.get('/api/ratings', async () => {
        await gate;
        return HttpResponse.json({ error: { code: 'unauthorized', message: 'not logged in' } }, { status: 401 });
      }),
    );
    renderApp('/login');
    await screen.findByRole('link', { name: 'Log in' });
    const stale = api.get('/api/ratings').catch(() => undefined); // sent while logged out
    await user.type(screen.getByLabelText('Username'), 'alice');
    await user.type(screen.getByLabelText('Password'), 'correct horse battery');
    await user.click(within(screen.getByRole('main')).getByRole('button', { name: 'Log in' }));
    await screen.findByRole('button', { name: /alice/ });
    await act(async () => {
      release();
      await stale;
    });
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
  });

  it('a wrong password (401 invalid_credentials) does not end an existing session', async () => {
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    server.use(handlerFor('login', 'invalid_credentials'));
    await act(async () => {
      await api.post('/api/auth/login', { body: { username: 'x', password: 'y' } }).catch(() => undefined);
    });
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
  });

  it('logs out through POST /api/auth/logout and returns to the logged-out navigation', async () => {
    const user = userEvent.setup();
    const seen = countRequests('POST', '/api/auth/logout');
    const { router } = renderApp('/ratings');
    await user.click(await screen.findByRole('button', { name: /alice/ }));
    expect(screen.getByRole('button', { name: /alice/ })).toHaveAttribute('aria-expanded', 'true');
    await user.click(screen.getByRole('button', { name: 'Log out' }));
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
    expect(seen).toHaveLength(1);
    expect(seen[0]?.headers.get('X-Requested-With')).toBe('recs');
    expect(router.state.location.pathname).toBe('/login');
  });

  it('still logs out locally if the session had already expired (401)', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('logout', 'unauthorized'));
    renderApp('/');
    await user.click(await screen.findByRole('button', { name: /alice/ }));
    await user.click(screen.getByRole('button', { name: 'Log out' }));
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
  });

  it('stays logged in and says so when logout fails for another reason', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('logout', 'csrf_rejected'));
    const { router } = renderApp('/ratings');
    await user.click(await screen.findByRole('button', { name: /alice/ }));
    await user.click(screen.getByRole('button', { name: 'Log out' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Your request was blocked. Reload the page and try again.');
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Log in' })).not.toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/ratings');
  });

  it('closes the account menu with Escape and returns focus to its button', async () => {
    const user = userEvent.setup();
    renderApp('/');
    const button = await screen.findByRole('button', { name: /alice/ });
    await user.click(button);
    expect(screen.getByRole('button', { name: 'Log out' })).toBeVisible();
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('button', { name: 'Log out' })).not.toBeInTheDocument();
    expect(button).toHaveAttribute('aria-expanded', 'false');
    expect(button).toHaveFocus();
  });
});

describe('model-unavailable banner', () => {
  it('appears on any 503 model_unavailable response, can be dismissed, and other pages keep working', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('recommend', 'model_unavailable'));
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    expect(screen.queryByText(BANNER, { exact: false })).not.toBeInTheDocument();
    // The live region is mounted (empty) before the error, so screen readers announce the change.
    const region = screen.getByRole('status');
    expect(region).toBeEmptyDOMElement();

    await act(async () => {
      await api.get('/api/recommend').catch(() => undefined);
    });
    const banner = await screen.findByRole('status');
    expect(banner).toBe(region);
    expect(banner).toHaveTextContent(BANNER);

    // Navigation and the rest of the site still work while the banner is up.
    await user.click(within(screen.getByRole('navigation', { name: 'Main' })).getByRole('link', { name: 'How it works' }));
    expect(screen.getByRole('heading', { level: 1, name: 'How it works' })).toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent(BANNER);

    await user.click(screen.getByRole('button', { name: 'Dismiss' }));
    expect(screen.queryByText(BANNER, { exact: false })).not.toBeInTheDocument();
    expect(region).toBeEmptyDOMElement();
  });

  it.each([
    ['another endpoint (getMovie)', 'getMovie' as const],
    ['putRating', 'putRating' as const],
  ])('also appears for %s', async (_, op) => {
    server.use(handlerFor(op, 'model_unavailable'));
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    await act(async () => {
      if (op === 'getMovie') await api.get('/api/movies/{movieId}', { path: { movieId: 1 } }).catch(() => undefined);
      else await api.put('/api/ratings/{movieId}', { path: { movieId: 1 }, body: { rating: 4 } }).catch(() => undefined);
    });
    expect(await screen.findByText(BANNER, { exact: false })).toBeInTheDocument();
  });

  it('does not appear for other errors', async () => {
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    server.use(
      handlerFor('recommend', 'unknown_model'),
      http.get('/api/movies/:movieId', () => HttpResponse.json({ error: { code: 'internal', message: 'x' } }, { status: 503 })),
    );
    await act(async () => {
      await api.get('/api/recommend').catch(() => undefined);
      await api.get('/api/movies/{movieId}', { path: { movieId: 1 } }).catch(() => undefined);
    });
    expect(screen.queryByText(BANNER, { exact: false })).not.toBeInTheDocument();
  });

  it('has no axe violations while shown', async () => {
    server.use(handlerFor('recommend', 'model_unavailable'));
    renderApp('/');
    await screen.findByRole('button', { name: /alice/ });
    await act(async () => {
      await api.get('/api/recommend').catch(() => undefined);
    });
    await screen.findByRole('status');
    expect(await axeViolations()).toEqual([]);
  });
});
