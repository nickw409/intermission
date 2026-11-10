import { screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { queryOf, recordRequests } from '../../lib/test-helpers';
import { handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

async function openPage() {
  const app = renderApp('/compare');
  await screen.findByRole('region', { name: 'Implicit ALS' });
  return app;
}

function column(name: string): HTMLElement {
  return screen.getByRole('region', { name });
}

function entry(columnName: string, title: string): HTMLElement {
  const link = within(column(columnName)).getByRole('link', { name: title });
  const li = link.closest('li');
  if (!li) throw new Error(`no entry for ${title}`);
  return li;
}

describe('Compare page', () => {
  it('compares popularity, item-KNN and the default model at first, one column each', async () => {
    const compare = recordRequests('/api/recommend/compare');
    renderApp('/compare');
    expect(screen.getByRole('heading', { level: 1, name: 'Compare models' })).toBeInTheDocument();
    await screen.findByRole('region', { name: 'Implicit ALS' });
    expect(compare).toHaveLength(1);
    expect(queryOf(compare[0]!)).toEqual({ models: 'pop,knn,als', k: '10' });
    const headings = screen.getAllByRole('heading', { level: 2 }).map((h) => h.textContent);
    expect(headings).toEqual(expect.arrayContaining(['Popularity', 'Item-KNN', 'Implicit ALS']));
    const pop = column('Popularity');
    expect(within(pop).getAllByRole('listitem').map((li) => within(li).getByRole('link').textContent)).toEqual([
      'Forrest Gump',
      'The Silence of the Lambs',
      'Fight Club',
    ]);
    expect(within(pop).getByRole('img', { name: 'Forrest Gump (1994)' })).toHaveAttribute('src', '/api/posters/356');
  });

  it('highlights movies that appear in two or more columns, and says so in text', async () => {
    await openPage();
    // Fight Club is in pop and als; LOTR and Empire Strikes Back are in knn and als.
    const fightClub = entry('Popularity', 'Fight Club');
    expect(fightClub).toHaveClass('compare-item--shared');
    expect(fightClub).toHaveTextContent('Also in Implicit ALS');
    expect(entry('Implicit ALS', 'Fight Club')).toHaveTextContent('Also in Popularity');
    expect(entry('Item-KNN', 'Star Wars: Episode V - The Empire Strikes Back')).toHaveTextContent('Also in Implicit ALS');
    const gump = entry('Popularity', 'Forrest Gump');
    expect(gump).not.toHaveClass('compare-item--shared');
    expect(gump).not.toHaveTextContent('Also in');
    expect(screen.getByText('3 movies appear in more than one list.')).toBeInTheDocument();
  });

  it('offers only the models the server has loaded, and refetches when the choice changes', async () => {
    const user = userEvent.setup();
    await openPage();
    const picker = screen.getByRole('group', { name: 'Models to compare' });
    const boxes = within(picker).getAllByRole('checkbox');
    expect(boxes.map((b) => b.getAttribute('value'))).toEqual(['pop', 'pop_count', 'knn', 'content', 'mf', 'als']);
    expect(boxes.filter((b) => (b as HTMLInputElement).checked).map((b) => b.getAttribute('value'))).toEqual([
      'pop',
      'knn',
      'als',
    ]);
    const compare = recordRequests('/api/recommend/compare');
    await user.click(within(picker).getByRole('checkbox', { name: 'Matrix factorization' }));
    await waitFor(() => expect(compare).toHaveLength(1));
    expect(queryOf(compare[0]!).models).toBe('pop,knn,mf,als');
  });

  it('keeps between 3 and 5 models selected', async () => {
    const user = userEvent.setup();
    await openPage();
    const picker = screen.getByRole('group', { name: 'Models to compare' });
    // At 3, the checked boxes can't be unchecked.
    expect(within(picker).getByRole('checkbox', { name: 'Popularity' })).toBeDisabled();
    expect(within(picker).getByRole('checkbox', { name: 'Content-based' })).toBeEnabled();
    await user.click(within(picker).getByRole('checkbox', { name: 'Content-based' }));
    await user.click(within(picker).getByRole('checkbox', { name: 'Matrix factorization' }));
    // At 5, the unchecked box can't be checked.
    expect(within(picker).getByRole('checkbox', { name: 'Popularity (count)' })).toBeDisabled();
    expect(within(picker).getByRole('checkbox', { name: 'Popularity' })).toBeEnabled();
    expect(picker).toHaveAccessibleDescription('Choose 3 to 5 models.');
  });

  it('in cold start shows the single popularity list the server sends, with a prompt to rate more', async () => {
    // recs-api answers a cold-start compare with ONE list (pop), whatever models were asked for.
    server.use(
      http.get('/api/recommend/compare', () =>
        HttpResponse.json({
          model_version: 'v',
          cold_start: true,
          results: [
            { model: 'pop', items: [{ movieId: 356, title: 'Forrest Gump', year: 1994, score: 4.1, because: [] }] },
          ],
        }),
      ),
    );
    renderApp('/compare');
    expect(await screen.findByText(/Rate at least 5 movies to compare models/)).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Rate more movies' })).toHaveAttribute('href', '/onboarding');
    expect(screen.getByRole('region', { name: 'Popularity' })).toBeInTheDocument();
    expect(screen.queryByRole('region', { name: 'Implicit ALS' })).not.toBeInTheDocument();
    expect(screen.queryByText(/more than one list/)).not.toBeInTheDocument();    expect(await axeViolations()).toEqual([]);
  });

  it('says when a column has no movies', async () => {
    server.use(
      http.get('/api/recommend/compare', () =>
        HttpResponse.json({ model_version: 'v', cold_start: false, results: [{ model: 'pop', items: [] }] }),
      ),
    );
    renderApp('/compare');
    expect(await screen.findByText('No movies from this model.')).toBeInTheDocument();
  });

  it('shows the banner and an error with retry when the model service is down', async () => {
    const user = userEvent.setup();
    server.use(handlerFor('compare', 'model_unavailable'));
    renderApp('/compare');
    const alert = await within(screen.getByRole('main')).findByRole('alert');
    expect(alert).toHaveTextContent('Recommendations are temporarily unavailable');
    await waitFor(() =>
      expect(screen.getAllByRole('status').some((s) => /Everything else still works/.test(s.textContent ?? ''))).toBe(
        true,
      ),
    );
    server.use(handlerFor('compare', 'ok'));
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('region', { name: 'Implicit ALS' })).toBeInTheDocument();
  });

  it('shows an error when the model list cannot load', async () => {
    server.use(handlerFor('listModels', 'model_unavailable'));
    const compare = recordRequests('/api/recommend/compare');
    renderApp('/compare');
    expect(await within(screen.getByRole('main')).findByRole('alert')).toHaveTextContent(
      'Recommendations are temporarily unavailable',
    );
    expect(compare).toHaveLength(0);
  });

  it('asks a signed-out visitor to log in', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderApp('/compare');
    expect(await screen.findByRole('link', { name: /Log in to compare/ })).toBeInTheDocument();
  });

  it('has no axe violations', async () => {
    await openPage();
    expect(await axeViolations()).toEqual([]);
  });

  it('has no axe violations when the model service is down', async () => {
    server.use(handlerFor('compare', 'model_unavailable'));
    renderApp('/compare');
    await within(screen.getByRole('main')).findByRole('alert');
    expect(await axeViolations()).toEqual([]);
  });
});
