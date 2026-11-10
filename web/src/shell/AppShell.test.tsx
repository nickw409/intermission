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

  it('has no axe violations when signed in, signed out, and with the menu open', async () => {
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

  it('treats an unreachable server as logged out', async () => {
    server.use(http.get('/api/me', () => HttpResponse.error()));
    renderApp('/');
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
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

    await act(async () => {
      await api.get('/api/recommend').catch(() => undefined);
    });
    const banner = await screen.findByRole('status');
    expect(banner).toHaveTextContent(BANNER);

    // Navigation and the rest of the site still work while the banner is up.
    await user.click(within(screen.getByRole('navigation', { name: 'Main' })).getByRole('link', { name: 'How it works' }));
    expect(screen.getByRole('heading', { level: 1, name: 'How it works' })).toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent(BANNER);

    await user.click(screen.getByRole('button', { name: 'Dismiss' }));
    expect(screen.queryByText(BANNER, { exact: false })).not.toBeInTheDocument();
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
    expect(await screen.findByRole('status')).toHaveTextContent(BANNER);
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
