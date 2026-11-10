/* eslint-disable @typescript-eslint/no-explicit-any -- the tests reshape fixture JSON freely */
// A chart that fails to draw, or a chart bundle that fails to load, must show an inline error with
// a retry, never a blank page.
import { screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

const flags = vi.hoisted(() => ({ failNdcgPlot: false, failChartsModule: false }));

vi.mock('@observablehq/plot', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@observablehq/plot')>();
  return {
    ...actual,
    plot: (options: any) => {
      if (flags.failNdcgPlot && options?.x?.label === 'NDCG@10 (mean, 95% CI) →') throw new Error('plot failed');
      return actual.plot(options);
    },
  };
});

vi.mock('./EvaluationCharts', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./EvaluationCharts')>();
  function MaybeBroken(props: Parameters<typeof actual.default>[0]) {
    if (flags.failChartsModule) throw new Error('chunk failed');
    return actual.default(props);
  }
  return { default: MaybeBroken };
});

const reportFiles = import.meta.glob<Record<string, any>>('../../../../contracts/fixtures/reports/*.json', {
  eager: true,
  import: 'default',
});
const reportsById = new Map(Object.values(reportFiles).map((r) => [r.run_id as string, r]));

beforeEach(() => {
  vi.spyOn(console, 'error').mockImplementation(() => {}); // React logs caught render errors
  server.use(http.get('/api/reports/:runId', ({ params }) => HttpResponse.json(reportsById.get(String(params.runId)))));
});

afterEach(() => {
  flags.failNdcgPlot = false;
  flags.failChartsModule = false;
  vi.restoreAllMocks();
});

describe('evaluation page error boundaries', () => {
  it('replaces only the chart that failed with an error and a retry', async () => {
    flags.failNdcgPlot = true;
    renderApp('/evaluation');
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent('Couldn\'t draw the chart "NDCG@10 by model".');
    // The other charts and the page around them are still there.
    expect(await screen.findByRole('img', { name: 'Recall@k by model' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1, name: 'Evaluation' })).toBeInTheDocument();
    expect(screen.getByRole('navigation', { name: 'Main' })).toBeInTheDocument();

    flags.failNdcgPlot = false;
    await userEvent.setup().click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('img', { name: 'NDCG@10 by model' })).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('shows an error with a retry when the chart bundle fails, keeping the rest of the page', async () => {
    flags.failChartsModule = true;
    renderApp('/evaluation');
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent("The charts couldn't be shown.");
    expect(screen.getByRole('heading', { level: 1, name: 'Evaluation' })).toBeInTheDocument();
    expect(screen.getByRole('region', { name: 'Reports not shown' })).toBeInTheDocument();

    flags.failChartsModule = false;
    await userEvent.setup().click(within(alert).getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('img', { name: 'NDCG@10 by model' })).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });
});
