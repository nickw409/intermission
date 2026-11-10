/* eslint-disable @typescript-eslint/no-explicit-any -- the tests reshape fixture JSON freely */
import { screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { fixtureFor } from '../../mocks/handlers';
import { axeViolations } from '../../test/axe';
import { renderApp } from '../../test/render';
import { server } from '../../test/setup';

// Every run report fixture, keyed by run_id, served by GET /api/reports/:runId.
const reportFiles = import.meta.glob<Record<string, any>>('../../../../contracts/fixtures/reports/*.json', {
  eager: true,
  import: 'default',
});
const reportsById = new Map(Object.values(reportFiles).map((r) => [r.run_id as string, r]));
const listing = fixtureFor('listReports').cases[0]!.response.body as { items: Record<string, any>[] };

const ALS_K128 = '2026-11-09T22-02-10Z_als_k128';
const ALS_K64 = '2026-11-09T21-14-03Z_als';
const POP_V2 = '2026-12-01T00-00-00Z_pop_v2';

function serveReports(overrides: Record<string, () => Response> = {}) {
  const requested: string[] = [];
  server.use(
    http.get('/api/reports/:runId', ({ params }) => {
      const id = String(params.runId);
      requested.push(id);
      if (overrides[id]) return overrides[id]();
      const body = reportsById.get(id);
      if (!body) return HttpResponse.json({ error: { code: 'not_found', message: 'no such report' } }, { status: 404 });
      return HttpResponse.json(structuredClone(body));
    }),
  );
  return requested;
}

function serveListing(items: Record<string, any>[]) {
  server.use(http.get('/api/reports', () => HttpResponse.json({ items })));
}

async function openPage() {
  const app = renderApp('/evaluation');
  await screen.findByRole('img', { name: 'NDCG@10 by model' });
  return app;
}

/** The figure (chart + controls + data table) whose chart has this accessible name. */
function figure(name: string | RegExp): HTMLElement {
  const svg = screen.getByRole('img', { name });
  const fig = svg.closest('figure');
  if (!fig) throw new Error('chart is not inside a <figure>');
  return fig;
}

function dataTable(name: string | RegExp): HTMLElement {
  return within(figure(name)).getByRole('table', { hidden: true });
}

/** A per_k value read from a fixture, formatted as the tables show it. */
function perK(runId: string, curve: 'ndcg' | 'recall' | 'precision', k: number): string {
  const point = (reportsById.get(runId)!.per_k[curve] as [number, number][]).find(([x]) => x === k);
  if (!point) throw new Error(`no ${curve}@${k} in ${runId}`);
  return point[1].toFixed(4);
}

/** "translate(x,y)" -> [x, y] */
function translation(element: Element): [number, number] {
  const match = /translate\(([-\d.e]+),([-\d.e]+)\)/.exec(element.getAttribute('transform') ?? '');
  if (!match) throw new Error(`no translate on ${element.outerHTML}`);
  return [Number(match[1]), Number(match[2])];
}

/** Rebuilds the chart's linear x scale from two of its axis tick labels. */
function xScaleOf(svg: Element): (value: number) => number {
  const ticks = [...svg.querySelectorAll('[data-mark="x-axis tick label"] text')].map(
    (t) => [Number(t.textContent), translation(t)[0]] as const,
  );
  const [first, last] = [ticks[0]!, ticks.at(-1)!];
  const slope = (last[1] - first[1]) / (last[0] - first[0]);
  return (value) => first[1] + (value - first[0]) * slope;
}

/** The y position of each y-axis label (model name -> y). */
function yRowsOf(svg: Element): Map<string, number> {
  return new Map(
    [...svg.querySelectorAll('[data-mark="y-axis tick label"] text')].map((t) => [t.textContent!, translation(t)[1]]),
  );
}

function rowFor(table: HTMLElement, header: string): HTMLElement {
  const cell = within(table).getByRole('rowheader', { name: header, hidden: true });
  return cell.closest('tr')!;
}

describe('evaluation page', () => {
  it('shows a loading state, then every chart as an SVG with an accessible title and description', async () => {
    serveReports();
    renderApp('/evaluation');
    expect(screen.getByRole('heading', { level: 1, name: 'Evaluation' })).toBeInTheDocument();
    expect(await within(screen.getByRole('main')).findByRole('status')).toHaveTextContent(/Loading/);

    await screen.findByRole('img', { name: 'NDCG@10 by model' });
    const names = [
      'NDCG@10 by model',
      'Recall@k by model',
      'Coverage@10 vs NDCG@10',
      'Training curve: als (objective)',
      'Training curve: mf (val_rmse)',
      'NDCG@10: cold users vs all users',
    ];
    for (const name of names) {
      const svg = screen.getByRole('img', { name });
      expect(svg.tagName.toLowerCase()).toBe('svg');
      expect(svg).toHaveAccessibleDescription(/.{20,}/);
      expect(svg.querySelectorAll('path, circle, line, rect, g').length).toBeGreaterThan(0);
    }
    expect(within(screen.getByRole('main')).queryByRole('status')).not.toBeInTheDocument();
  });

  it('gives every chart a data table with a "Show data" toggle that stays available to screen readers', async () => {
    serveReports();
    await openPage();
    const user = userEvent.setup();
    const fig = figure('Recall@k by model');
    const toggle = within(fig).getByRole('button', { name: 'Show data' });
    const table = within(fig).getByRole('table', { hidden: true });
    // Collapsed: visually hidden, but still in the accessibility tree.
    expect(toggle).toHaveAttribute('aria-expanded', 'false');
    expect(table.closest('.visually-hidden')).not.toBeNull();
    expect(screen.getAllByRole('table').length).toBeGreaterThanOrEqual(6);
    await user.click(toggle);
    expect(within(fig).getByRole('button', { name: 'Hide data' })).toHaveAttribute('aria-expanded', 'true');
    expect(table.closest('.visually-hidden')).toBeNull();

    for (const svg of screen.getAllByRole('img')) {
      const owner = svg.closest('figure');
      if (!owner || svg.tagName.toLowerCase() !== 'svg') continue;
      expect(within(owner).getByRole('table', { hidden: true })).toBeInTheDocument();
      expect(within(owner).getByRole('button', { name: /Show data|Hide data/ })).toBeInTheDocument();
    }
  });

  it('charts NDCG@10 with 95% CIs per model, using the newest run of each model, with popularity as reference', async () => {
    serveReports();
    await openPage();
    const table = dataTable('NDCG@10 by model');
    const als = rowFor(table, 'als');
    expect(als).toHaveTextContent('0.0902'); // newest als run is k=128
    expect(als).toHaveTextContent('0.0893');
    expect(als).toHaveTextContent('0.0911');
    expect(rowFor(table, 'pop')).toHaveTextContent('0.0412');
    expect(rowFor(table, 'pop')).toHaveTextContent('baseline');
    // The six models, each once.
    expect(within(table).getAllByRole('rowheader', { hidden: true }).map((c) => c.textContent)).toEqual([
      'pop',
      'pop_count',
      'knn',
      'content',
      'mf',
      'als',
    ]);
    expect(screen.getByRole('img', { name: 'NDCG@10 by model' })).toHaveAccessibleDescription(/popularity/i);
  });

  it('labels models whose NDCG@10 CI does not clear popularity\'s as "does not beat popularity"', async () => {
    serveReports();
    await openPage();
    const table = dataTable('NDCG@10 by model');
    expect(rowFor(table, 'content')).toHaveTextContent('does not beat popularity');
    for (const model of ['pop_count', 'knn', 'mf', 'als']) {
      expect(rowFor(table, model)).not.toHaveTextContent('does not beat popularity');
      expect(rowFor(table, model)).toHaveTextContent('beats popularity');
    }
    // Visible outside the table too, not by color alone.
    const fig = figure('NDCG@10 by model');
    expect(within(fig).getByText(/content does not beat popularity/)).toBeVisible();
  });

  it('labels a model as failing when its CI overlaps popularity\'s', async () => {
    const knn = structuredClone(reportsById.get('2026-11-09T21-10-00Z_knn')!);
    knn.metrics['ndcg@10'] = { mean: 0.0425, ci95: [0.0415, 0.0435] };
    serveReports({ '2026-11-09T21-10-00Z_knn': () => HttpResponse.json(knn) });
    await openPage();
    expect(rowFor(dataTable('NDCG@10 by model'), 'knn')).toHaveTextContent('does not beat popularity');
  });

  it('charts Recall@k curves with one line per model and a table of every point', async () => {
    serveReports();
    await openPage();
    const table = dataTable('Recall@k by model');
    const k50 = rowFor(table, '50');
    expect(k50).toHaveTextContent(perK(ALS_K128, 'recall', 50)); // newest als run
    expect(k50).toHaveTextContent(perK('2026-11-09T21-00-00Z_pop', 'recall', 50));
    expect(rowFor(table, '10')).toHaveTextContent(perK('2026-11-09T21-12-00Z_content', 'recall', 10));
    const headers = within(table)
      .getAllByRole('columnheader', { hidden: true })
      .map((c) => c.textContent);
    expect(headers).toEqual(['k', 'pop', 'pop_count', 'knn', 'content', 'mf', 'als']);
    const svg = screen.getByRole('img', { name: 'Recall@k by model' });
    expect(svg.querySelectorAll('[data-mark="line"] path').length).toBe(6);
  });

  it('charts coverage against NDCG@10', async () => {
    serveReports();
    await openPage();
    const table = dataTable('Coverage@10 vs NDCG@10');
    expect(rowFor(table, 'content')).toHaveTextContent('0.1534');
    expect(rowFor(table, 'content')).toHaveTextContent('0.0198');
    expect(rowFor(table, 'als')).toHaveTextContent('0.0507');
  });

  it('charts training curves labelled with their metric and notes models without one', async () => {
    serveReports();
    await openPage();
    const table = dataTable('Training curve: als (objective)');
    expect(within(table).getAllByRole('columnheader', { hidden: true }).map((c) => c.textContent)).toEqual([
      'Iteration',
      'objective',
    ]);
    expect(rowFor(table, '1')).toHaveTextContent('147,600,000');
    expect(rowFor(table, '15')).toHaveTextContent('108,009,178.5');
    expect(within(dataTable('Training curve: mf (val_rmse)')).getAllByRole('row', { hidden: true }).length).toBe(21);
    expect(screen.getByText(/No training curve for pop, pop_count, knn and content/)).toBeInTheDocument();
  });

  it('charts the cold-user slice against all users per model', async () => {
    serveReports();
    await openPage();
    const table = dataTable('NDCG@10: cold users vs all users');
    const als = rowFor(table, 'als');
    expect(als).toHaveTextContent('0.0902');
    expect(als).toHaveTextContent('0.0705');
    expect(screen.getByRole('img', { name: 'NDCG@10: cold users vs all users' })).toHaveAccessibleDescription(
      /fewer than 20 training ratings/,
    );
  });

  it('shows a dash and a note when a cold-user slice has no estimate', async () => {
    const knn = structuredClone(reportsById.get('2026-11-09T21-10-00Z_knn')!);
    knn.slices.cold_users['ndcg@10'] = null;
    serveReports({ '2026-11-09T21-10-00Z_knn': () => HttpResponse.json(knn) });
    await openPage();
    expect(rowFor(dataTable('NDCG@10: cold users vs all users'), 'knn')).toHaveTextContent('—');
    expect(screen.getByText(/No cold-user estimate for knn/)).toBeInTheDocument();
  });

  it('compares two runs of the same model side by side, labelled by their config', async () => {
    serveReports();
    await openPage();
    const user = userEvent.setup();
    const section = screen.getByRole('region', { name: 'Compare two runs' });
    const model = within(section).getByRole('combobox', { name: 'Model' });
    // Only models with at least two runs can be compared.
    expect(within(model).getAllByRole('option').map((o) => o.textContent)).toEqual(['als']);
    const runA = within(section).getByRole('combobox', { name: 'Run A' });
    const runB = within(section).getByRole('combobox', { name: 'Run B' });
    expect(within(runA).getAllByRole('option').map((o) => o.textContent)).toEqual(['als · k=128', 'als · k=64']);
    expect(runA).toHaveValue(ALS_K128);
    expect(runB).toHaveValue(ALS_K64);

    const headline = within(section).getByRole('table', { name: 'Headline metrics' });
    expect(within(headline).getAllByRole('columnheader').map((c) => c.textContent)).toEqual([
      'Metric',
      'als · k=128',
      'als · k=64',
    ]);
    const ndcg = rowFor(headline, 'NDCG@10');
    expect(ndcg).toHaveTextContent('0.0902');
    expect(ndcg).toHaveTextContent('0.0874');
    expect(rowFor(headline, 'Coverage@10')).toHaveTextContent('0.0442');

    expect(within(section).getByRole('img', { name: 'Recall@k: als · k=128 vs als · k=64' })).toBeInTheDocument();
    expect(within(section).getByRole('img', { name: 'Training curve: als · k=128 vs als · k=64 (objective)' })).toBeInTheDocument();

    // Swap the order: the table follows the picker.
    await user.selectOptions(runA, ALS_K64);
    await user.selectOptions(runB, ALS_K128);
    expect(within(headline).getAllByRole('columnheader').map((c) => c.textContent)).toEqual([
      'Metric',
      'als · k=64',
      'als · k=128',
    ]);
  });

  it('explains when no model has two runs to compare', async () => {
    serveReports();
    serveListing(listing.items.filter((i) => i.run_id !== ALS_K64));
    await openPage();
    const section = screen.getByRole('region', { name: 'Compare two runs' });
    expect(within(section).getByText(/at least two runs of the same model/)).toBeInTheDocument();
  });

  it('shows listing entries with an unknown schema_version as unsupported and never fetches or charts them', async () => {
    const requested = serveReports();
    await openPage();
    const notice = screen.getByRole('region', { name: 'Reports not shown' });
    expect(within(notice).getByText(POP_V2)).toBeInTheDocument();
    expect(notice).toHaveTextContent('unsupported report format');
    expect(requested).not.toContain(POP_V2);
    expect(requested).toHaveLength(7);
    // pop appears once (the v1 run), not twice.
    expect(within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { name: 'pop', hidden: true })).toHaveLength(1);
  });

  it('refuses a report body with an unknown schema_version instead of guessing', async () => {
    // The listing claims v1, but the body is v2: the page must not try to read it.
    const v2 = structuredClone(reportsById.get(POP_V2)!);
    v2.run_id = '2026-11-09T21-12-00Z_content';
    serveReports({ '2026-11-09T21-12-00Z_content': () => HttpResponse.json(v2) });
    await openPage();
    expect(
      screen.getByText(/This report uses a newer format \(v2\) that this site can't display/),
    ).toBeInTheDocument();
    expect(within(dataTable('NDCG@10 by model')).queryByRole('rowheader', { name: 'content', hidden: true })).toBeNull();
  });

  it('skips a run whose report is malformed or fails to load, and says so', async () => {
    const broken = structuredClone(reportsById.get('2026-11-09T21-13-00Z_mf')!);
    delete broken.per_k;
    serveReports({
      '2026-11-09T21-13-00Z_mf': () => HttpResponse.json(broken),
      '2026-11-09T21-10-00Z_knn': () =>
        HttpResponse.json({ error: { code: 'not_found', message: 'gone' } }, { status: 404 }),
    });
    await openPage();
    const problems = screen.getByRole('region', { name: 'Reports not shown' });
    expect(problems).toHaveTextContent('2026-11-09T21-13-00Z_mf');
    expect(problems).toHaveTextContent(/per_k/);
    expect(problems).toHaveTextContent('2026-11-09T21-10-00Z_knn');
    expect(problems).toHaveTextContent("We couldn't find that.");
    const rows = within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { hidden: true });
    expect(rows.map((c) => c.textContent)).toEqual(['pop', 'pop_count', 'content', 'als']);
  });

  it('shows an empty state when there are no reports', async () => {
    serveReports();
    serveListing([]);
    renderApp('/evaluation');
    expect(await screen.findByText(/No evaluation runs yet/)).toBeInTheDocument();
    expect(screen.queryByRole('img')).not.toBeInTheDocument();
  });

  it('shows an empty state when every report is in an unsupported format', async () => {
    serveReports();
    serveListing(listing.items.filter((i) => i.run_id === POP_V2));
    renderApp('/evaluation');
    expect(await screen.findByText(/No evaluation runs this site can display/)).toBeInTheDocument();
    expect(screen.getByRole('region', { name: 'Reports not shown' })).toHaveTextContent(POP_V2);
  });

  it('shows an error with a retry when the listing fails', async () => {
    serveReports();
    let calls = 0;
    server.use(
      http.get('/api/reports', () => {
        calls += 1;
        if (calls === 1) return HttpResponse.json({ error: { code: 'internal', message: 'x' } }, { status: 500 });
        return HttpResponse.json(listing);
      }),
    );
    renderApp('/evaluation');
    expect(await screen.findByRole('alert')).toHaveTextContent('Something went wrong. Please try again.');
    await userEvent.setup().click(screen.getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('img', { name: 'NDCG@10 by model' })).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('only charts runs from one dataset and split at a time', async () => {
    const small = structuredClone(reportsById.get('2026-11-09T21-00-00Z_pop')!);
    small.run_id = 'small_pop';
    small.dataset = 'ml-latest-small';
    small.created_at = '2026-11-10T00:00:00Z';
    reportsById.set('small_pop', small);
    try {
      serveReports();
      const smallItem = { ...listing.items.find((i) => i.model === 'pop')!, run_id: 'small_pop', dataset: 'ml-latest-small', created_at: small.created_at };
      serveListing([smallItem, ...listing.items]);
      renderApp('/evaluation');
      const picker = await screen.findByRole('combobox', { name: 'Dataset and split' });
      // Newest run's dataset first.
      expect(picker).toHaveValue('ml-latest-small · test');
      await screen.findByRole('img', { name: 'NDCG@10 by model' });
      expect(within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { hidden: true })).toHaveLength(1);
      await userEvent.setup().selectOptions(picker, 'ml-32m · test');
      await waitFor(() =>
        expect(within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { hidden: true })).toHaveLength(6),
      );
    } finally {
      reportsById.delete('small_pop');
    }
  });

  it('draws the NDCG@10 chart from the right numbers: reference rule, whiskers, means and failure labels', async () => {
    serveReports();
    await openPage();
    const svg = screen.getByRole('img', { name: 'NDCG@10 by model' });
    const x = xScaleOf(svg);
    const rows = yRowsOf(svg);
    const latest = ['2026-11-09T21-00-00Z_pop', '2026-11-09T21-05-00Z_pop_count', '2026-11-09T21-10-00Z_knn',
      '2026-11-09T21-12-00Z_content', '2026-11-09T21-13-00Z_mf', ALS_K128].map((id) => reportsById.get(id)!);
    expect([...rows.keys()]).toEqual(latest.map((r) => r.model));
    const close = (actual: number, expected: number) => expect(Math.abs(actual - expected)).toBeLessThan(0.5);

    // Popularity reference rule at pop's mean, spanning the plot.
    const reference = svg.querySelectorAll('.ndcg-reference line');
    expect(reference).toHaveLength(1);
    close(Number(reference[0]!.getAttribute('x1')), x(latest[0]!.metrics['ndcg@10'].mean));
    close(Number(reference[0]!.getAttribute('x2')), x(latest[0]!.metrics['ndcg@10'].mean));

    // One whisker per model, from CI low to CI high, on that model's row.
    const whiskers = [...svg.querySelectorAll('.ndcg-ci line')];
    expect(whiskers).toHaveLength(latest.length);
    latest.forEach((report, i) => {
      const [lo, hi] = report.metrics['ndcg@10'].ci95 as [number, number];
      const line = whiskers[i]!;
      close(Number(line.getAttribute('x1')), x(lo));
      close(Number(line.getAttribute('x2')), x(hi));
      close(Number(line.getAttribute('y1')), rows.get(report.model)!);
    });

    // One mean marker per model, at the mean.
    const means = [...svg.querySelectorAll('.ndcg-mean > *')];
    expect(means).toHaveLength(latest.length);
    latest.forEach((report, i) => {
      const marker = means[i]!;
      const [cx, cy] = marker.tagName === 'circle'
        ? [Number(marker.getAttribute('cx')), Number(marker.getAttribute('cy'))]
        : translation(marker);
      close(cx, x(report.metrics['ndcg@10'].mean));
      close(cy, rows.get(report.model)!);
    });

    // In-chart failure label only on content's row, just right of its CI.
    const labels = [...svg.querySelectorAll('.ndcg-verdict text')];
    expect(labels.map((t) => t.textContent)).toEqual(['does not beat popularity']);
    const [labelX, labelY] = translation(labels[0]!);
    close(labelX, x(reportsById.get('2026-11-09T21-12-00Z_content')!.metrics['ndcg@10'].ci95[1]));
    close(labelY, rows.get('content')!);
  });

  it('skips a report whose body is for a different run than the one listed', async () => {
    const requested = serveReports({
      '2026-11-09T21-10-00Z_knn': () => HttpResponse.json(structuredClone(reportsById.get('2026-11-09T21-13-00Z_mf'))),
    });
    await openPage();
    expect(requested).toContain('2026-11-09T21-10-00Z_knn');
    const problems = screen.getByRole('region', { name: 'Reports not shown' });
    expect(problems).toHaveTextContent('2026-11-09T21-10-00Z_knn');
    expect(problems).toHaveTextContent(/returned run 2026-11-09T21-13-00Z_mf instead/);
    const rows = within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { hidden: true });
    expect(rows.map((c) => c.textContent)).toEqual(['pop', 'pop_count', 'content', 'mf', 'als']);
  });

  it('fetches and charts a run listed twice only once', async () => {
    const requested = serveReports();
    const knn = listing.items.find((i) => i.model === 'knn')!;
    serveListing([...listing.items, knn]);
    await openPage();
    expect(requested.filter((id) => id === knn.run_id)).toHaveLength(1);
    expect(screen.getByRole('region', { name: 'Reports not shown' })).toHaveTextContent(/listed more than once/);
    expect(within(dataTable('NDCG@10 by model')).getAllByRole('rowheader', { name: 'knn', hidden: true })).toHaveLength(1);
  });

  it.each(['..', '../secrets', 'a b', ''])('never requests a listed run_id that is not a valid id (%j)', async (badId) => {
    const requested: string[] = [];
    server.events.on('request:start', ({ request }) => {
      const path = new URL(request.url).pathname;
      if (path !== '/api/reports' && path.startsWith('/api/reports')) requested.push(path);
    });
    serveReports();
    const knn = listing.items.find((i) => i.model === 'knn')!;
    serveListing([...listing.items.filter((i) => i !== knn), { ...knn, run_id: badId }]);
    await openPage();
    expect(requested).toHaveLength(6);
    expect(requested.every((path) => /^\/api\/reports\/[A-Za-z0-9_-]+$/.test(path))).toBe(true);
    expect(screen.getByRole('region', { name: 'Reports not shown' })).toHaveTextContent(/not a valid run id/);
  });

  it('explains a v1 listing entry that has no summary, distinctly from an unknown version', async () => {
    const requested = serveReports();
    const knn = listing.items.find((i) => i.model === 'knn')!;
    serveListing([...listing.items.filter((i) => i !== knn), { ...knn, model: null, headline: null }]);
    await openPage();
    const problems = screen.getByRole('region', { name: 'Reports not shown' });
    expect(problems).toHaveTextContent(/2026-11-09T21-10-00Z_knn: the listing has no model or headline metrics for this run/);
    expect(problems).toHaveTextContent(/2026-12-01T00-00-00Z_pop_v2: unsupported report format \(v2\)/);
    expect(requested).not.toContain(knn.run_id);
  });

  it('has no axe violations with all charts shown and with tables expanded', async () => {
    serveReports();
    await openPage();
    expect(await axeViolations()).toEqual([]);
    const user = userEvent.setup();
    for (const button of screen.getAllByRole('button', { name: 'Show data' })) await user.click(button);
    expect(await axeViolations()).toEqual([]);
  });
});
