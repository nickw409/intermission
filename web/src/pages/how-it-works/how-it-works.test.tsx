import { screen, within } from '@testing-library/react';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { errorBody, gate } from '../../lib/test-helpers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const MODELS = ['Popularity', 'Content-based', 'Item-KNN', 'Matrix factorization', 'Implicit ALS'];

function section(name: string): HTMLElement {
  return screen.getByRole('region', { name });
}

describe('How it works page', () => {
  it('explains each model and links to its Evaluation results', async () => {
    renderApp('/how-it-works');
    expect(screen.getByRole('heading', { level: 1, name: 'How it works' })).toBeInTheDocument();
    for (const name of MODELS) {
      const s = section(name);
      expect(within(s).getByRole('link', { name: `See ${name} results on the Evaluation page` })).toHaveAttribute(
        'href',
        '/evaluation',
      );
    }
    expect(section('Item-KNN')).toHaveTextContent(/same users/);
    expect(section('Implicit ALS')).toHaveTextContent(/alternating least squares/i);
    expect(screen.getByRole('main')).toHaveTextContent(/Fold-in/);
    await screen.findAllByText(/NDCG@10/);
  });

  it("shows each model's latest headline NDCG@10 from the run reports", async () => {
    renderApp('/how-it-works');
    // listReports fixture: the newest ALS run is the k = 128 one (0.0902).
    expect(await within(section('Implicit ALS')).findByText(/NDCG@10 0\.090/)).toBeInTheDocument();
    expect(within(section('Implicit ALS')).getByText(/ml-32m/)).toBeInTheDocument();
    expect(within(section('Popularity')).getByText(/NDCG@10 0\.0/)).toBeInTheDocument();
  });

  it('shows the explanations at once and a loading note for each result while the reports load', async () => {
    const { wait, release } = gate();
    server.use(
      http.get('/api/reports', async () => {
        await wait;
        return HttpResponse.json({ items: [] });
      }),
    );
    renderApp('/how-it-works');
    expect(section('Item-KNN')).toHaveTextContent(/same users/);
    for (const name of MODELS) expect(within(section(name)).getByText('Loading results…')).toBeInTheDocument();
    release();
    expect(await within(section('Implicit ALS')).findByText('Not evaluated yet.')).toBeInTheDocument();
    expect(screen.queryByText('Loading results…')).not.toBeInTheDocument();
  });

  it('says when a model has not been evaluated yet', async () => {
    server.use(http.get('/api/reports', () => HttpResponse.json({ items: [] })));
    renderApp('/how-it-works');
    expect(await within(section('Implicit ALS')).findByText('Not evaluated yet.')).toBeInTheDocument();
  });

  it('keeps the explanations when the reports cannot load', async () => {
    server.use(http.get('/api/reports', () => errorBody('internal', 500)));
    renderApp('/how-it-works');
    expect(await screen.findByText(/Results couldn't be loaded/)).toBeInTheDocument();
    expect(section('Item-KNN')).toHaveTextContent(/same users/);
  });

  it('has no axe violations', async () => {
    renderApp('/how-it-works');
    await within(section('Implicit ALS')).findByText(/NDCG@10 0\.090/);
    expect(await axeViolations()).toEqual([]);
  });
});
