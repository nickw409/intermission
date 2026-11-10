import { act, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { modelServiceDown, queryOf, rateWithKeyboard, recordRequests } from '../../lib/test-helpers';
import { fixtureFor, handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const LOTR = 'The Lord of the Rings: The Fellowship of the Ring';
const matrix = fixtureFor('getMovie').cases.find((c) => c.name === 'ok_rated_by_caller')!.response
  .body as Record<string, unknown>;

async function openMovie(id = 4993, title = LOTR) {
  const app = renderApp(`/movies/${id}`);
  await screen.findByRole('heading', { level: 1, name: title });
  return app;
}

function similarRow(): HTMLElement {
  return screen.getByRole('region', { name: 'Similar movies' });
}

describe('Movie page', () => {
  it('shows a loading state, then the details', async () => {
    renderApp('/movies/4993');
    expect(screen.getByText('Loading movie…')).toBeInTheDocument();
    await screen.findByRole('heading', { level: 1, name: LOTR });
    const main = screen.getByRole('main');
    expect(within(main).getByRole('img', { name: `${LOTR} (2001)` })).toHaveAttribute('src', '/api/posters/4993');
    expect(main).toHaveTextContent('2001');
    expect(main).toHaveTextContent('Adventure');
    expect(main).toHaveTextContent('Fantasy');
    expect(main).toHaveTextContent('Average 4.11 from 198 ratings');
  });

  it("shows the signed-in user's rating and saves changes optimistically", async () => {
    server.use(http.get('/api/movies/:movieId', () => HttpResponse.json(matrix)));
    const puts = recordRequests('/api/ratings/2571', 'PUT');
    await openMovie(2571, 'The Matrix');
    const slider = screen.getByRole('slider', { name: 'Your rating for The Matrix' });
    expect(slider).toHaveAttribute('aria-valuetext', '4.5 stars');
    rateWithKeyboard(slider, 3);
    expect(slider).toHaveAttribute('aria-valuetext', '3 stars');
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(await puts[0]!.json()).toEqual({ rating: 3 });
  });

  it('still saves a rating while the model service is down (similar row failing)', async () => {
    server.use(http.get('/api/movies/:movieId', () => HttpResponse.json(matrix)));
    const puts = recordRequests('/api/ratings/2571', 'PUT');
    await openMovie(2571, 'The Matrix');
    modelServiceDown();
    // Switching the similar row now hits the dead model service.
    await userEvent.setup().click(screen.getByRole('button', { name: 'Similar content' }));
    expect(await within(similarRow()).findByRole('alert')).toHaveTextContent('Recommendations are temporarily unavailable');
    const slider = screen.getByRole('slider', { name: 'Your rating for The Matrix' });
    rateWithKeyboard(slider, 2);
    await waitFor(() => expect(puts).toHaveLength(1));
    await act(async () => {});
    expect(slider).toHaveAttribute('aria-valuetext', '2 stars');
    expect(screen.queryByText(/Couldn't save your rating/)).not.toBeInTheDocument();
  });

  it('rolls the rating back when saving fails', async () => {
    server.use(http.get('/api/movies/:movieId', () => HttpResponse.json(matrix)), handlerFor('putRating', 'csrf_rejected'));
    await openMovie(2571, 'The Matrix');
    const slider = screen.getByRole('slider', { name: 'Your rating for The Matrix' });
    rateWithKeyboard(slider, 1);
    expect(await screen.findByText(/Couldn't save your rating/)).toBeInTheDocument();
    expect(slider).toHaveAttribute('aria-valuetext', '4.5 stars');
  });

  it('shows similar movies from item-KNN, and switches to the content model', async () => {
    const user = userEvent.setup();
    const similar = recordRequests(/^\/api\/movies\/\d+\/similar$/);
    await openMovie();
    const row = similarRow();
    await within(row).findByRole('link', { name: 'The Matrix' });
    expect(queryOf(similar[0]!)).toEqual({ model: 'knn', k: '12' });
    expect(within(row).getByRole('img', { name: 'Star Wars: Episode V - The Empire Strikes Back (1980)' })).toBeInTheDocument();
    const knn = within(row).getByRole('button', { name: 'Rated by the same people' });
    const content = within(row).getByRole('button', { name: 'Similar content' });
    expect(knn).toHaveAttribute('aria-pressed', 'true');
    expect(content).toHaveAttribute('aria-pressed', 'false');
    await user.click(content);
    expect(content).toHaveAttribute('aria-pressed', 'true');
    await waitFor(() => expect(similar).toHaveLength(2));
    expect(queryOf(similar[1]!).model).toBe('content');
  });

  it('keeps the details when the similar row fails, and says why', async () => {
    server.use(handlerFor('getSimilar', 'unknown_model'));
    await openMovie();
    expect(await within(similarRow()).findByRole('alert')).toHaveTextContent("That model isn't available.");
  });

  it('says when there are no similar movies', async () => {
    server.use(
      http.get('/api/movies/:movieId/similar', () =>
        HttpResponse.json({ model: 'knn', model_version: 'v', movieId: 4993, items: [] }),
      ),
    );
    await openMovie();
    expect(await within(similarRow()).findByText('No similar movies found.')).toBeInTheDocument();
  });

  it('says the movie was not found (404), without a rating control', async () => {
    server.use(handlerFor('getMovie', 'not_found'));
    renderApp('/movies/999999999');
    expect(await screen.findByRole('heading', { level: 1, name: 'Movie not found' })).toBeInTheDocument();
    expect(screen.queryByRole('slider')).not.toBeInTheDocument();
  });

  it('does not call the API for an id that is not a movie id', async () => {
    const movies = recordRequests(/^\/api\/movies\//);
    renderApp('/movies/abc');
    expect(await screen.findByRole('heading', { level: 1, name: 'Movie not found' })).toBeInTheDocument();
    await screen.findByRole('button', { name: /alice/ });
    expect(movies).toHaveLength(0);
  });

  it('shows the banner and a retry when the model service is down', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('getMovie', 'model_unavailable'));
    renderApp('/movies/4993');
    const alert = await within(screen.getByRole('main')).findByRole('alert');
    expect(alert).toHaveTextContent('Recommendations are temporarily unavailable');
    server.use(handlerFor('getMovie', 'ok'));
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('heading', { level: 1, name: LOTR })).toBeInTheDocument();
  });

  it('invites a signed-out visitor to log in to rate', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    await openMovie();
    expect(await screen.findByRole('link', { name: /Log in to rate this movie/ })).toBeInTheDocument();
    expect(screen.queryByRole('slider')).not.toBeInTheDocument();
  });

  it('has no axe violations', async () => {
    await openMovie();
    await within(similarRow()).findByRole('link', { name: 'The Matrix' });
    expect(await axeViolations()).toEqual([]);
  });

  it('has no axe violations when not found', async () => {
    server.use(handlerFor('getMovie', 'not_found'));
    renderApp('/movies/999999999');
    await screen.findByRole('heading', { level: 1, name: 'Movie not found' });
    expect(await axeViolations()).toEqual([]);
  });
});
