// The overview charts. Each takes the newest run of every model in one dataset/split group.
// Identity is never carried by color alone: models sit on their own row (position), or get their
// own marker shape and line dash, and the legend and tooltips name them.
import * as Plot from '@observablehq/plot';
import { useMemo } from 'react';
import type { RunReport } from '../../api/client';
import { BASELINE_MODEL, modelStyle, popularityVerdict } from '../../lib/reports/models';
import { ChartFigure, modelColor, type LegendItem, type PlotContext, type TableData } from './ChartFigure';
import { interval, metric, MISSING, number } from './format';

type Estimate = RunReport['metrics']['ndcg@10'];

const lo = (e: Estimate) => Math.min(...e.ci95);
const hi = (e: Estimate) => Math.max(...e.ci95);

function legendFor(runs: RunReport[], options: Partial<LegendItem> = {}): LegendItem[] {
  return runs.map((run) => ({ label: run.model, model: run.model, ...options }));
}

/** "a, b and c" */
export function listing(items: string[]): string {
  if (items.length <= 1) return items.join('');
  return `${items.slice(0, -1).join(', ')} and ${items.at(-1)}`;
}

// ---- 1. NDCG@10 by model ----

export type Verdict = 'baseline' | 'beats' | 'fails' | 'unknown';

export function verdicts(runs: RunReport[]): Map<string, Verdict> {
  const pop = runs.find((run) => run.model === BASELINE_MODEL);
  return new Map(
    runs.map((run) => {
      if (run.model === BASELINE_MODEL) return [run.model, 'baseline'];
      if (!pop) return [run.model, 'unknown'];
      return [run.model, popularityVerdict(run.metrics['ndcg@10'], pop.metrics['ndcg@10'])];
    }),
  );
}

const VERDICT_TEXT: Record<Verdict, string> = {
  baseline: 'baseline',
  beats: 'beats popularity',
  fails: 'does not beat popularity',
  unknown: 'no popularity run to compare against',
};

function ndcgPlot(runs: RunReport[], { dark }: PlotContext): Plot.PlotOptions {
  const verdict = verdicts(runs);
  const pop = runs.find((run) => run.model === BASELINE_MODEL);
  const hasFailures = [...verdict.values()].includes('fails');
  const models = runs.map((run) => run.model);
  const maxX = Math.max(...runs.map((run) => hi(run.metrics['ndcg@10'])));
  const marks: Plot.Markish[] = [Plot.gridX({ strokeOpacity: 0.08 })];
  if (pop) {
    const popMean = pop.metrics['ndcg@10'].mean;
    marks.push(
      Plot.ruleX([popMean], { stroke: 'currentColor', strokeOpacity: 0.55, strokeDasharray: '4,4' }),
      Plot.text([popMean], {
        x: (d: number) => d,
        frameAnchor: 'top',
        dy: -14,
        text: () => 'popularity',
        fill: 'currentColor',
      }),
    );
  }
  for (const run of runs) {
    const e = run.metrics['ndcg@10'];
    const style = modelStyle(run.model);
    const color = modelColor(run.model, dark);
    marks.push(
      Plot.ruleY([run], { y: 'model', x1: () => lo(e), x2: () => hi(e), stroke: color, strokeWidth: 2 }),
      Plot.dot([run], { y: 'model', x: () => e.mean, r: 5, symbol: style.symbol, fill: color }),
    );
    if (verdict.get(run.model) === 'fails') {
      marks.push(
        Plot.text([run], {
          y: 'model',
          x: () => hi(e),
          dx: 10,
          textAnchor: 'start',
          text: () => 'does not beat popularity',
          fill: 'currentColor',
        }),
      );
    }
  }
  marks.push(
    Plot.tip(
      runs,
      Plot.pointerY({
        y: 'model',
        x: (run: RunReport) => run.metrics['ndcg@10'].mean,
        title: (run: RunReport) =>
          `${run.model}\nNDCG@10 ${metric(run.metrics['ndcg@10'].mean)}\n95% CI ${interval(run.metrics['ndcg@10'].ci95)}\n${VERDICT_TEXT[verdict.get(run.model) ?? 'unknown']}`,
      }),
    ),
  );
  return {
    height: 40 + 36 * runs.length,
    marginTop: 28,
    marginLeft: 80,
    marginRight: hasFailures ? 170 : 20,
    x: { domain: [0, maxX * 1.05], label: 'NDCG@10 (mean, 95% CI) →', labelAnchor: 'left' },
    y: { domain: models, label: null },
    marks,
  };
}

export function NdcgChart({ runs }: { runs: RunReport[] }) {
  const verdict = useMemo(() => verdicts(runs), [runs]);
  const pop = runs.find((run) => run.model === BASELINE_MODEL);
  const failing = runs.filter((run) => verdict.get(run.model) === 'fails').map((run) => run.model);
  const best = runs.reduce((a, b) => (b.metrics['ndcg@10'].mean > a.metrics['ndcg@10'].mean ? b : a));


  const table: TableData = {
    columns: ['Model', 'NDCG@10', '95% CI', 'Against popularity'],
    rows: runs.map((run) => ({
      header: run.model,
      cells: [
        metric(run.metrics['ndcg@10'].mean),
        interval(run.metrics['ndcg@10'].ci95),
        VERDICT_TEXT[verdict.get(run.model) ?? 'unknown'],
      ],
    })),
  };

  const description =
    `Mean NDCG@10 with 95% confidence intervals for ${runs.length} models. ` +
    (pop
      ? `The dashed line marks the popularity baseline at ${metric(pop.metrics['ndcg@10'].mean)}; a model beats popularity only if its whole interval lies above popularity's. `
      : 'There is no popularity run in this group, so no model is judged against it. ') +
    `Highest: ${best.model} at ${metric(best.metrics['ndcg@10'].mean)}.`;

  return (
    <ChartFigure
      title="NDCG@10 by model"
      description={description}
      build={ndcgPlot}
      input={runs}
      table={table}
      notes={
        failing.length > 0 && (
          <p className="chart__note">
            <span aria-hidden="true">⚠ </span>
            {failing.map((m) => `${m} does not beat popularity`).join('; ')} (its 95% CI is not entirely above
            popularity&apos;s), so it counts as a failure.
          </p>
        )
      }
    />
  );
}

// ---- 2. Recall@k ----

function recallByModelPlot(runs: RunReport[], { dark }: PlotContext): Plot.PlotOptions {
  return recallPlot(
    runs.map((run) => ({ run, label: run.model, dash: modelStyle(run.model).dash })),
    dark,
    'Recall@k',
  );
}

export function RecallChart({ runs }: { runs: RunReport[] }) {
  const ks = [...new Set(runs.flatMap((run) => run.per_k.recall.map(([k]) => k)))].sort((a, b) => a - b);


  const table: TableData = {
    columns: ['k', ...runs.map((run) => run.model)],
    rows: ks.map((k) => ({
      header: String(k),
      cells: runs.map((run) => {
        const point = run.per_k.recall.find(([x]) => x === k);
        return point ? metric(point[1]) : MISSING;
      }),
    })),
  };

  return (
    <ChartFigure
      title="Recall@k by model"
      description={`Share of each user's relevant held-out movies found in the top k, for k from ${ks[0] ?? 1} to ${ks.at(-1) ?? 50}, one line per model.`}
      build={recallByModelPlot}
      input={runs}
      table={table}
      legend={legendFor(runs, { line: true })}
    />
  );
}

export interface CurveSeries {
  run: RunReport;
  label: string;
  dash: string;
  hollow?: boolean;
}

/** Recall@k lines; shared with the run picker. */
export function recallPlot(series: CurveSeries[], dark: boolean, yLabel: string): Plot.PlotOptions {
  const points = series.flatMap(({ run, label }) => run.per_k.recall.map(([k, value]) => ({ label, k, value })));
  const marks: Plot.Markish[] = [Plot.ruleY([0], { strokeOpacity: 0.3 })];
  for (const { run, dash, hollow } of series) {
    const color = modelColor(run.model, dark);
    const data = run.per_k.recall;
    marks.push(
      Plot.line(data, { x: '0', y: '1', stroke: color, strokeWidth: 2, strokeDasharray: dash }),
      Plot.dot(data, {
        x: '0',
        y: '1',
        r: 4,
        symbol: modelStyle(run.model).symbol,
        fill: hollow ? 'none' : color,
        stroke: color,
        strokeWidth: hollow ? 1.5 : 0,
      }),
    );
  }
  marks.push(
    Plot.tip(
      points,
      Plot.pointer({ x: 'k', y: 'value', title: (d: (typeof points)[number]) => `${d.label}\nk = ${d.k}\n${yLabel} ${metric(d.value)}` }),
    ),
  );
  return {
    height: 320,
    marginLeft: 56,
    x: { label: 'k (list length) →', domain: [0, Math.max(50, ...points.map((p) => p.k))] },
    y: { label: `↑ ${yLabel}`, grid: true, zero: true },
    marks,
  };
}

// ---- 3. Coverage vs NDCG ----

function coveragePlot(runs: RunReport[], { dark }: PlotContext): Plot.PlotOptions {
  const logScale = runs.every((run) => run.aggregates['coverage@10'] > 0);
  const marks: Plot.Markish[] = [];
  for (const run of runs) {
    const e = run.metrics['ndcg@10'];
    const color = modelColor(run.model, dark);
    const x = () => run.aggregates['coverage@10'];
    marks.push(
      Plot.ruleX([run], { x, y1: () => lo(e), y2: () => hi(e), stroke: color, strokeWidth: 2 }),
      Plot.dot([run], { x, y: () => e.mean, r: 6, symbol: modelStyle(run.model).symbol, fill: color }),
    );
  }
  marks.push(
    Plot.tip(
      runs,
      Plot.pointer({
        x: (run: RunReport) => run.aggregates['coverage@10'],
        y: (run: RunReport) => run.metrics['ndcg@10'].mean,
        title: (run: RunReport) =>
          `${run.model}\nCoverage@10 ${metric(run.aggregates['coverage@10'])}\nNDCG@10 ${metric(run.metrics['ndcg@10'].mean)}`,
      }),
    ),
  );
  return {
    height: 340,
    marginLeft: 56,
    x: logScale
      ? { type: 'log', label: 'Coverage@10 (share of catalog, log scale) →', grid: true, ticks: 4, tickFormat: '.2~%' }
      : { label: 'Coverage@10 (share of catalog) →', grid: true, zero: true, tickFormat: '.2~%' },
    y: { label: '↑ NDCG@10', grid: true, zero: true },
    marks,
  };
}

export function CoverageChart({ runs }: { runs: RunReport[] }) {
  const widest = runs.reduce((a, b) => (b.aggregates['coverage@10'] > a.aggregates['coverage@10'] ? b : a));


  const table: TableData = {
    columns: ['Model', 'Coverage@10', 'NDCG@10', 'NDCG@10 95% CI'],
    rows: runs.map((run) => ({
      header: run.model,
      cells: [metric(run.aggregates['coverage@10']), metric(run.metrics['ndcg@10'].mean), interval(run.metrics['ndcg@10'].ci95)],
    })),
  };

  return (
    <ChartFigure
      title="Coverage@10 vs NDCG@10"
      description={`Accuracy against diversity: each model's NDCG@10 (with 95% CI) against the share of the catalog that appears in anyone's top 10. ${widest.model} covers the most, at ${metric(widest.aggregates['coverage@10'])}.`}
      build={coveragePlot}
      input={runs}
      table={table}
      legend={legendFor(runs)}
    />
  );
}

// ---- 4. Training curves ----

export function trainingPlot(series: CurveSeries[], dark: boolean, metricName: string): Plot.PlotOptions {
  const points = series.flatMap(({ run, label }) => run.training_curve.map(([iteration, value]) => ({ label, iteration, value })));
  const marks: Plot.Markish[] = series.map(({ run, dash }) =>
    Plot.line(run.training_curve, { x: '0', y: '1', stroke: modelColor(run.model, dark), strokeWidth: 2, strokeDasharray: dash }),
  );
  marks.push(
    Plot.tip(
      points,
      Plot.pointer({
        x: 'iteration',
        y: 'value',
        title: (d: (typeof points)[number]) => `${d.label}\niteration ${d.iteration}\n${metricName} ${number(d.value)}`,
      }),
    ),
  );
  const largest = Math.max(0, ...points.map((p) => Math.abs(p.value)));
  return {
    height: 260,
    marginLeft: 72,
    x: { label: 'Iteration →' },
    // Losses run to hundreds of millions, where SI suffixes ("145M") keep ticks short; for small
    // values such as RMSE they would read "880m", so they are only used for large ones.
    y: { label: `↑ ${metricName}`, grid: true, ...(largest >= 1e4 ? { tickFormat: '~s' } : {}) },
    marks,
  };
}

function singleTrainingPlot(run: RunReport, { dark }: PlotContext): Plot.PlotOptions {
  return trainingPlot([{ run, label: run.model, dash: '' }], dark, run.training_curve_metric);
}

function TrainingFigure({ run }: { run: RunReport }) {
  const curve = run.training_curve;
  const first = curve[0]!;
  const last = curve.at(-1)!;
  return (
    <ChartFigure
      headingLevel={4}
      title={`Training curve: ${run.model} (${run.training_curve_metric})`}
      description={`${run.training_curve_metric} over ${curve.length} iterations of ${run.model}, from ${number(first[1])} at iteration ${first[0]} to ${number(last[1])} at iteration ${last[0]}.`}
      build={singleTrainingPlot}
      input={run}
      table={{
        columns: ['Iteration', run.training_curve_metric],
        rows: curve.map(([iteration, value]) => ({ header: String(iteration), cells: [number(value)] })),
      }}
    />
  );
}

export function TrainingCurves({ runs }: { runs: RunReport[] }) {
  const withCurve = runs.filter((run) => run.training_curve.length > 0);
  const without = runs.filter((run) => run.training_curve.length === 0).map((run) => run.model);
  return (
    <div className="chart-group">
      <h3>Training curves</h3>
      <p className="muted">
        Each model logs its own measure (a loss or a validation metric), so each gets its own chart and scale.
      </p>
      {without.length > 0 && (
        <p className="chart__note">
          No training curve for {listing(without)}: {without.length === 1 ? 'it is' : 'they are'} not trained iteratively.
        </p>
      )}
      <div className="chart-grid">
        {withCurve.map((run) => (
          <TrainingFigure key={run.run_id} run={run} />
        ))}
      </div>
    </div>
  );
}

// ---- 5. Cold users vs all users ----

function coldUsersPlot(runs: RunReport[], { dark }: PlotContext): Plot.PlotOptions {
  const marks: Plot.Markish[] = [Plot.gridX({ strokeOpacity: 0.08 })];
  const points: { model: string; slice: string; value: number }[] = [];
  for (const run of runs) {
    const all = run.metrics['ndcg@10'].mean;
    const cold = run.slices.cold_users['ndcg@10']?.mean;
    const color = modelColor(run.model, dark);
    const symbol = modelStyle(run.model).symbol;
    points.push({ model: run.model, slice: 'All users', value: all });
    if (cold !== undefined) {
      points.push({ model: run.model, slice: 'Cold users', value: cold });
      marks.push(
        Plot.ruleY([run], { y: 'model', x1: () => Math.min(all, cold), x2: () => Math.max(all, cold), stroke: color, strokeOpacity: 0.6 }),
        Plot.dot([run], { y: 'model', x: () => cold, r: 5, symbol, fill: 'none', stroke: color, strokeWidth: 2 }),
      );
    }
    marks.push(Plot.dot([run], { y: 'model', x: () => all, r: 5, symbol, fill: color }));
  }
  marks.push(
    Plot.tip(
      points,
      Plot.pointer({ y: 'model', x: 'value', title: (d: (typeof points)[number]) => `${d.model}\n${d.slice}: NDCG@10 ${metric(d.value)}` }),
    ),
  );
  return {
    height: 40 + 36 * runs.length,
    marginLeft: 80,
    x: { label: 'NDCG@10 →', zero: true },
    y: { domain: runs.map((run) => run.model), label: null },
    marks,
  };
}

export function ColdUsersChart({ runs }: { runs: RunReport[] }) {
  const threshold = runs[0]?.slices.cold_users.train_ratings_lt ?? 0;
  const missing = runs.filter((run) => run.slices.cold_users['ndcg@10'] === null).map((run) => run.model);


  const table: TableData = {
    columns: ['Model', 'All users', 'All users 95% CI', 'Cold users', 'Cold users 95% CI', 'Cold users (count)'],
    rows: runs.map((run) => {
      const cold = run.slices.cold_users['ndcg@10'];
      return {
        header: run.model,
        cells: [
          metric(run.metrics['ndcg@10'].mean),
          interval(run.metrics['ndcg@10'].ci95),
          cold ? metric(cold.mean) : MISSING,
          cold ? interval(cold.ci95) : MISSING,
          number(run.slices.cold_users.users),
        ],
      };
    }),
  };

  return (
    <ChartFigure
      title="NDCG@10: cold users vs all users"
      description={`For each model, NDCG@10 over all evaluated users (filled marker) and over cold users, those with fewer than ${threshold} training ratings (hollow marker).`}
      build={coldUsersPlot}
      input={runs}
      table={table}
      legend={[
        { label: 'All users (filled)', model: runs[0]?.model ?? BASELINE_MODEL },
        { label: `Cold users, fewer than ${threshold} training ratings (hollow)`, model: runs[0]?.model ?? BASELINE_MODEL, hollow: true },
      ]}
      notes={
        missing.length > 0 && (
          <p className="chart__note">No cold-user estimate for {listing(missing)}: the slice had no users with a relevant held-out movie.</p>
        )
      }
    />
  );
}
