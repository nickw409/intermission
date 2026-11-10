import { render, screen } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { describe, expect, it } from 'vitest';
import { handlerFor } from './mocks/handlers';
import { routes } from './routes';
import { server } from './test/setup';

function renderAt(path: string) {
  render(<RouterProvider router={createMemoryRouter(routes, { initialEntries: [path] })} />);
}

describe('app shell', () => {
  it('renders navigation, the For you page and the signed-in user from the getMe fixture', async () => {
    renderAt('/');
    expect(screen.getByRole('navigation', { name: 'Main' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: 'For you' })).toBeInTheDocument();
    expect(await screen.findByText('alice')).toBeInTheDocument();
  });

  it('shows a log-in link when getMe returns 401', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderAt('/');
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
  });

  it.each([
    ['/login', 'Log in'],
    ['/signup', 'Sign up'],
    ['/onboarding', 'Rate some movies'],
    ['/compare', 'Compare models'],
    ['/movies/4993', 'Movie 4993'],
    ['/ratings', 'My ratings'],
    ['/evaluation', 'Evaluation'],
    ['/how-it-works', 'How it works'],
  ])('routes %s to its page', async (path, heading) => {
    renderAt(path);
    expect(screen.getByRole('heading', { level: 1, name: heading })).toBeInTheDocument();
    await screen.findByText('alice'); // let the shell's request settle
  });
});
