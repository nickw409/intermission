import { screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

async function openLoggedOut() {
  server.use(handlerFor('getMe', 'unauthorized'));
  const app = renderApp('/login');
  await screen.findByRole('link', { name: 'Log in' }); // shell has settled as logged out
  return app;
}

function captureLogins() {
  const bodies: unknown[] = [];
  server.events.on('request:start', async ({ request }) => {
    if (request.method === 'POST' && new URL(request.url).pathname === '/api/auth/login') {
      bodies.push(await request.clone().json());
    }
  });
  return bodies;
}

async function fill(username: string, password: string) {
  const user = userEvent.setup();
  if (username) await user.type(screen.getByLabelText('Username'), username);
  if (password) await user.type(screen.getByLabelText('Password'), password);
  await user.click(screen.getByRole('button', { name: 'Log in' }));
}

describe('login page', () => {
  it('requires both fields and sends nothing until they are filled', async () => {
    await openLoggedOut();
    const bodies = captureLogins();
    await fill('', '');
    expect(screen.getByLabelText('Username')).toHaveAccessibleDescription('Enter your username.');
    expect(screen.getByLabelText('Password')).toHaveAccessibleDescription('Enter your password.');
    expect(screen.getByLabelText('Username')).toHaveAttribute('aria-invalid', 'true');
    expect(screen.getByLabelText('Username')).toHaveFocus();
    expect(bodies).toEqual([]);
  });

  it('rejects values longer than the contract allows (username 64, password 128)', async () => {
    await openLoggedOut();
    const bodies = captureLogins();
    await fill('u'.repeat(65), 'p'.repeat(129));
    expect(screen.getByLabelText('Username')).toHaveAccessibleDescription('Usernames are at most 64 characters.');
    expect(screen.getByLabelText('Password')).toHaveAccessibleDescription('Passwords are at most 128 characters.');
    expect(bodies).toEqual([]);
  });

  it('sends the credentials and goes to onboarding when the user has fewer than 10 ratings', async () => {
    const { router } = await openLoggedOut();
    const bodies = captureLogins();
    await fill('alice', 'correct horse battery'); // login fixture: rating_count 5
    expect(await screen.findByRole('heading', { level: 1, name: 'Rate some movies' })).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/onboarding');
    expect(bodies).toEqual([{ username: 'alice', password: 'correct horse battery' }]);
    // The shell now shows the signed-in user without another round trip.
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
  });

  it('goes to For you when the user already has 10 or more ratings', async () => {
    const { router } = await openLoggedOut();
    server.use(
      http.post('/api/auth/login', () => HttpResponse.json({ username: 'bob', rating_count: 10, created_at: 1794256680 })),
    );
    await fill('bob', 'correct horse battery');
    expect(await screen.findByRole('heading', { level: 1, name: 'For you' })).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/');
  });

  it.each([
    ['invalid_credentials', 'Incorrect username or password.'],
    ['rate_limited', 'Too many attempts. Please wait a minute and try again.'],
    ['csrf_rejected', 'Your request was blocked. Reload the page and try again.'],
  ])('shows a friendly message for %s', async (caseName, message) => {
    const { router } = await openLoggedOut();
    server.use(handlerFor('login', caseName));
    await fill('alice', 'wrong password!');
    expect(await screen.findByRole('alert')).toHaveTextContent(message);
    expect(router.state.location.pathname).toBe('/login');
    expect(screen.getByRole('button', { name: 'Log in' })).toBeEnabled();
  });

  it('shows a connection message when the server cannot be reached', async () => {
    await openLoggedOut();
    server.use(http.post('/api/auth/login', () => HttpResponse.error()));
    await fill('alice', 'correct horse battery');
    expect(await screen.findByRole('alert')).toHaveTextContent(
      "Can't reach the server. Check your connection and try again.",
    );
  });

  it('links to sign up', async () => {
    await openLoggedOut();
    const main = screen.getByRole('main');
    expect(main.querySelector('a[href="/signup"]')).not.toBeNull();
  });

  it('has no axe violations, including with errors shown', async () => {
    await openLoggedOut();
    expect(await axeViolations()).toEqual([]);
    server.use(handlerFor('login', 'invalid_credentials'));
    await fill('', 'x');
    await fill('alice', '');
    await waitFor(() => expect(screen.getByRole('alert')).toBeInTheDocument());
    expect(await axeViolations()).toEqual([]);
  });
});
