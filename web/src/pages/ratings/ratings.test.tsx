import { screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { errorBody, gate, modelServiceDown, rateWithKeyboard, recordRequests } from '../../lib/test-helpers';
import { handlerFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const STAR_WARS = 'Star Wars: Episode IV - A New Hope';

async function openPage() {
  const app = renderApp('/ratings');
  await screen.findByRole('table');
  return app;
}

/** Movie titles in table order. */
function titles(): string[] {
  return within(screen.getByRole('table'))
    .getAllByRole('row')
    .slice(1)
    .map((row) => within(row).getByRole('link').textContent ?? '');
}

function row(title: string): HTMLElement {
  const r = within(screen.getByRole('table')).getByRole('link', { name: title }).closest('tr');
  if (!r) throw new Error(`no row for ${title}`);
  return r;
}

describe('My ratings page', () => {
  it('lists the ratings newest first, each with an editable star rating', async () => {
    renderApp('/ratings');
    expect(screen.getByRole('heading', { level: 1, name: 'My ratings' })).toBeInTheDocument();
    expect(await screen.findByText('Loading your ratings…')).toBeInTheDocument();
    await screen.findByRole('table');
    expect(titles()).toEqual([STAR_WARS, 'The Matrix', 'Pulp Fiction', 'Toy Story', 'The Shawshank Redemption']);
    expect(within(row('The Matrix')).getByRole('slider', { name: 'Your rating for The Matrix' })).toHaveAttribute(
      'aria-valuetext',
      '4.5 stars',
    );
    expect(within(row(STAR_WARS)).getByRole('link', { name: STAR_WARS })).toHaveAttribute('href', '/movies/260');
    expect(screen.getByRole('columnheader', { name: /Rated on/ })).toHaveAttribute('aria-sort', 'descending');
  });

  it('sorts by title, rating and date from the column headers', async () => {
    const user = userEvent.setup();
    await openPage();
    await user.click(screen.getByRole('button', { name: /Title/ }));
    // Sorted the way MovieLens files titles, so a leading "The" doesn't count.
    expect(titles()).toEqual(['The Matrix', 'Pulp Fiction', 'The Shawshank Redemption', STAR_WARS, 'Toy Story']);
    expect(screen.getByRole('columnheader', { name: /Title/ })).toHaveAttribute('aria-sort', 'ascending');
    expect(screen.getByRole('columnheader', { name: /Rated on/ })).not.toHaveAttribute('aria-sort');
    await user.click(screen.getByRole('button', { name: /Title/ }));
    expect(screen.getByRole('columnheader', { name: /Title/ })).toHaveAttribute('aria-sort', 'descending');
    expect(titles()[0]).toBe('Toy Story');

    await user.click(screen.getByRole('button', { name: /Rating/ }));
    expect(screen.getByRole('columnheader', { name: /Rating/ })).toHaveAttribute('aria-sort', 'descending');
    expect(titles()[0]).toBe(STAR_WARS);
    expect(titles().slice(-2).sort()).toEqual(['The Shawshank Redemption', 'Toy Story']);

    await user.click(screen.getByRole('button', { name: /Rated on/ }));
    expect(titles()[0]).toBe(STAR_WARS);
  });

  it('edits a rating inline, optimistically', async () => {
    const puts = recordRequests('/api/ratings/2571', 'PUT');
    await openPage();
    const slider = within(row('The Matrix')).getByRole('slider');
    rateWithKeyboard(slider, 2);
    expect(slider).toHaveAttribute('aria-valuetext', '2 stars');
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(await puts[0]!.json()).toEqual({ rating: 2 });
  });

  it('rolls an edit back and says why when saving fails', async () => {
    server.use(http.put('/api/ratings/:movieId', () => HttpResponse.error()));
    await openPage();
    const slider = within(row('The Matrix')).getByRole('slider');
    rateWithKeyboard(slider, 2);
    expect(await within(row('The Matrix')).findByRole('alert')).toHaveTextContent(
      "Couldn't save your rating. Can't reach the server.",
    );
    expect(slider).toHaveAttribute('aria-valuetext', '4.5 stars');
  });

  it('deletes a rating at once, and Undo puts it back', async () => {
    const user = userEvent.setup();
    const deletes = recordRequests('/api/ratings/2571', 'DELETE');
    const puts = recordRequests('/api/ratings/2571', 'PUT');
    await openPage();
    await user.click(within(row('The Matrix')).getByRole('button', { name: 'Delete rating for The Matrix' }));
    expect(titles()).not.toContain('The Matrix');
    await waitFor(() => expect(deletes).toHaveLength(1));
    const notice = await screen.findByText('Deleted your rating for The Matrix.');
    // Focus goes to the Undo button so keyboard users can undo straight away.
    const undo = screen.getByRole('button', { name: 'Undo' });
    await waitFor(() => expect(undo).toHaveFocus());
    expect(notice.closest('[role="status"]')).not.toBeNull();
    await user.click(undo);
    expect(titles()).toEqual([STAR_WARS, 'The Matrix', 'Pulp Fiction', 'Toy Story', 'The Shawshank Redemption']);
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(await puts[0]!.json()).toEqual({ rating: 4.5 });
    expect(screen.queryByText('Deleted your rating for The Matrix.')).not.toBeInTheDocument();
  });

  it('puts the row back and says why when a delete fails', async () => {
    const user = userEvent.setup();
    const { wait, release } = gate();
    server.use(
      http.delete('/api/ratings/:movieId', async () => {
        await wait;
        return errorBody('internal', 500);
      }),
    );
    await openPage();
    await user.click(within(row('The Matrix')).getByRole('button', { name: 'Delete rating for The Matrix' }));
    expect(titles()).not.toContain('The Matrix');
    release();
    expect(await screen.findByText("Couldn't delete your rating for The Matrix. Something went wrong. Please try again.")).toBeInTheDocument();
    expect(titles()).toContain('The Matrix');
  });

  it('clearing the stars deletes the rating too', async () => {
    const user = userEvent.setup();
    const deletes = recordRequests('/api/ratings/2571', 'DELETE');
    await openPage();
    await user.click(within(row('The Matrix')).getByRole('button', { name: 'Clear Your rating for The Matrix' }));
    await waitFor(() => expect(deletes).toHaveLength(1));
    expect(titles()).not.toContain('The Matrix');
    expect(screen.getByRole('button', { name: 'Undo' })).toBeInTheDocument();
  });

  it('keeps working while the model service is down', async () => {
    modelServiceDown();
    const puts = recordRequests('/api/ratings/2571', 'PUT');
    await openPage();
    rateWithKeyboard(within(row('The Matrix')).getByRole('slider'), 3);
    await waitFor(() => expect(puts).toHaveLength(1));
    expect(within(row('The Matrix')).queryByRole('alert')).not.toBeInTheDocument();
    expect(within(row('The Matrix')).getByRole('slider')).toHaveAttribute('aria-valuetext', '3 stars');
  });

  it('shows an empty state with a way to start rating', async () => {
    server.use(http.get('/api/ratings', () => HttpResponse.json({ items: [] })));
    renderApp('/ratings');
    expect(await screen.findByText("You haven't rated any movies yet.")).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Rate some movies' })).toHaveAttribute('href', '/onboarding');
  });

  it('shows an error with a retry when the list cannot load', async () => {
    const user = userEvent.setup();
    server.use(http.get('/api/ratings', () => errorBody('internal', 500)));
    renderApp('/ratings');
    const alert = await within(screen.getByRole('main')).findByRole('alert');
    expect(alert).toHaveTextContent('Something went wrong. Please try again.');
    server.use(handlerFor('listRatings', 'ok'));
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('table')).toBeInTheDocument();
  });

  it('asks a signed-out visitor to log in', async () => {
    server.use(handlerFor('getMe', 'unauthorized'));
    renderApp('/ratings');
    expect(await screen.findByRole('link', { name: /Log in to see your ratings/ })).toBeInTheDocument();
  });

  it('has no axe violations, including with the undo notice shown', async () => {
    const user = userEvent.setup();
    await openPage();
    expect(await axeViolations()).toEqual([]);
    await user.click(within(row('Toy Story')).getByRole('button', { name: 'Delete rating for Toy Story' }));
    await screen.findByText('Deleted your rating for Toy Story.');
    expect(await axeViolations()).toEqual([]);
  });
});
