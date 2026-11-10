import { screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it } from 'vitest';
import { handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

async function openLoggedOut() {
  server.use(handlerFor('getMe', 'unauthorized'));
  const app = renderApp('/signup');
  await screen.findByRole('link', { name: 'Log in' });
  return app;
}

function captureSignups() {
  const bodies: unknown[] = [];
  server.events.on('request:start', async ({ request }) => {
    if (request.method === 'POST' && new URL(request.url).pathname === '/api/auth/signup') {
      bodies.push(await request.clone().json());
    }
  });
  return bodies;
}

async function fill(fields: { username?: string; password?: string; invite?: string }) {
  const user = userEvent.setup();
  for (const [label, value] of [
    ['Username', fields.username],
    ['Password', fields.password],
    ['Invite code', fields.invite],
  ] as const) {
    const input = screen.getByLabelText(label);
    await user.clear(input);
    if (value) await user.type(input, value);
  }
  await user.click(screen.getByRole('button', { name: 'Sign up' }));
}

const USERNAME_RULE = 'Use 3–32 letters, digits, underscores, dots or hyphens.';
const PASSWORD_RULE = 'Use 10–128 characters.';

describe('signup page', () => {
  it('always shows the invite code field with its helper text', async () => {
    await openLoggedOut();
    expect(screen.getByLabelText('Invite code')).toHaveAccessibleDescription('Required while signup is invite-only');
  });

  it.each([
    ['ab', 'too short'],
    ['a'.repeat(33), 'too long'],
    ['a b', 'space'],
    ['bob!', 'punctuation outside [A-Za-z0-9_.-]'],
    ['zoë', 'non-ASCII letter'],
  ])('rejects username %j (%s) without calling the server', async (username) => {
    await openLoggedOut();
    const bodies = captureSignups();
    await fill({ username, password: 'correct horse battery', invite: 'k7Qm2xVb9pLs' });
    expect(screen.getByLabelText('Username')).toHaveAccessibleDescription(USERNAME_RULE);
    expect(screen.getByLabelText('Username')).toHaveAttribute('aria-invalid', 'true');
    expect(bodies).toEqual([]);
  });

  it.each([
    ['p'.repeat(9), 'too short'],
    ['p'.repeat(129), 'too long'],
  ])('rejects a password that is %s', async (password) => {
    await openLoggedOut();
    const bodies = captureSignups();
    await fill({ username: 'carol', password, invite: 'k7Qm2xVb9pLs' });
    expect(screen.getByLabelText('Password')).toHaveAccessibleDescription(PASSWORD_RULE);
    expect(bodies).toEqual([]);
  });

  it('accepts the boundary values and sends the contract body', async () => {
    const { router } = await openLoggedOut();
    const bodies = captureSignups();
    const username = 'A_b.c-' + '9'.repeat(26); // 32 characters
    await fill({ username, password: 'p'.repeat(10), invite: '  k7Qm2xVb9pLs ' });
    expect(await screen.findByRole('heading', { level: 1, name: 'Rate some movies' })).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/onboarding');
    expect(bodies).toEqual([{ username, password: 'p'.repeat(10), invite_code: 'k7Qm2xVb9pLs' }]);
  });

  it('accepts a 128-character password and omits a blank invite code', async () => {
    await openLoggedOut();
    const bodies = captureSignups();
    await fill({ username: 'abc', password: 'p'.repeat(128) });
    await screen.findByRole('heading', { level: 1, name: 'Rate some movies' });
    expect(bodies).toEqual([{ username: 'abc', password: 'p'.repeat(128) }]);
  });

  it.each([
    ['invalid_invite', "That invite code isn't valid. It may have expired or already been used."],
    ['username_taken', 'That username is already taken. Please choose another.'],
    ['rate_limited', 'Too many attempts. Please wait a minute and try again.'],
  ])('shows a friendly message for %s', async (caseName, message) => {
    const { router } = await openLoggedOut();
    server.use(handlerFor('signup', caseName));
    await fill({ username: 'carol', password: 'correct horse battery', invite: 'expired-code' });
    expect(await screen.findByRole('alert')).toHaveTextContent(message);
    expect(router.state.location.pathname).toBe('/signup');
  });

  it('has no axe violations, including with errors shown', async () => {
    await openLoggedOut();
    expect(await axeViolations()).toEqual([]);
    await fill({ username: 'a b', password: 'short' });
    expect(await axeViolations()).toEqual([]);
  });
});
