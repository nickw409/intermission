// Compare two runs of the same model (e.g. ALS with k = 64 against k = 128): headline metrics in a
// table, plus their Recall@k and training curves on shared axes. Run A is solid with filled
// markers, run B dashed with hollow markers, so the two never differ by color alone.
import { useCallback, useId, useState } from 'react';
import type { RunReport } from '../../api/client';
import { runLabels } from '../../lib/reports/models';
import { comparableRuns } from '../../lib/reports/select';
import { ChartFigure, DataTable, type PlotContext, type TableData } from './ChartFigure';
import { recallPlot, trainingPlot, type CurveSeries } from './charts';
import { interval, metric, MISSING, number } from './format';

const DASH_B = '6,3';

function estimateCell(e: { mean: number; ci95: number[] } | null, digits = 4): string {
  return e ? `${e.mean.toFixed(digits)} (${interval(e.ci95, digits)})` : MISSING;
}

const HEADLINE: { label: string; value: (run: RunReport) => string }[] = [
  { label: 'NDCG@10', value: (r) => estimateCell(r.metrics['ndcg@10']) },
  { label: 'Recall@10', value: (r) => estimateCell(r.metrics['recall@10']) },
  { label: 'Recall@50', value: (r) => estimateCell(r.metrics['recall@50']) },
  { label: 'Precision@10', value: (r) => estimateCell(r.metrics['precision@10']) },
  { label: 'MAP@10', value: (r) => estimateCell(r.metrics['map@10']) },
  { label: 'Mean popularity rank@10', value: (r) => estimateCell(r.metrics['mean_pop_rank@10'], 1) },
  { label: 'Coverage@10', value: (r) => metric(r.aggregates['coverage@10']) },
  { label: 'Cold-user NDCG@10', value: (r) => estimateCell(r.slices.cold_users['ndcg@10']) },
  { label: 'Fold-in Jaccard@10', value: (r) => (r['foldin_jaccard@10'] === null ? MISSING : metric(r['foldin_jaccard@10'])) },
  { label: 'Training time (s)', value: (r) => number(r.train_seconds) },
];

function Comparison({ a, b, labelA, labelB }: { a: RunReport; b: RunReport; labelA: string; labelB: string }) {
  const series: CurveSeries[] = [
    { run: a, label: labelA, dash: '' },
    { run: b, label: labelB, dash: DASH_B, hollow: true },
  ];
  const legend = [
    { label: labelA, model: a.model, line: true, dash: '' },
    { label: labelB, model: b.model, line: true, dash: DASH_B, hollow: true },
  ];
  const buildRecall = useCallback(
    ({ dark }: PlotContext) =>
      recallPlot(
        [
          { run: a, label: labelA, dash: '' },
          { run: b, label: labelB, dash: DASH_B, hollow: true },
        ],
        dark,
        'Recall@k',
      ),
    [a, b, labelA, labelB],
  );
  const sameCurveMetric =
    a.training_curve.length > 0 && b.training_curve.length > 0 && a.training_curve_metric === b.training_curve_metric;
  const buildTraining = useCallback(
    ({ dark }: PlotContext) =>
      trainingPlot(
        [
          { run: a, label: labelA, dash: '' },
          { run: b, label: labelB, dash: DASH_B },
        ],
        dark,
        a.training_curve_metric,
      ),
    [a, b, labelA, labelB],
  );

  const headline: TableData = {
    columns: ['Metric', labelA, labelB],
    rows: HEADLINE.map(({ label, value }) => ({ header: label, cells: [value(a), value(b)] })),
  };

  const curveTable = (pick: (run: RunReport) => [number, number][], xLabel: string): TableData => {
    const xs = [...new Set(series.flatMap(({ run }) => pick(run).map(([x]) => x)))].sort((p, q) => p - q);
    return {
      columns: [xLabel, labelA, labelB],
      rows: xs.map((x) => ({
        header: String(x),
        cells: [a, b].map((run) => {
          const point = pick(run).find(([px]) => px === x);
          return point ? (xLabel === 'k' ? metric(point[1]) : number(point[1])) : MISSING;
        }),
      })),
    };
  };

  return (
    <>
      <div className="table-scroll">
        <DataTable caption="Headline metrics" data={headline} />
      </div>
      <p className="muted">Means with 95% confidence intervals in brackets.</p>
      <div className="chart-grid">
        <ChartFigure
          headingLevel={3}
          title={`Recall@k: ${labelA} vs ${labelB}`}
          description={`Recall@k for the two runs: ${labelA} (solid line, filled markers) and ${labelB} (dashed line, hollow markers).`}
          build={buildRecall}
          table={curveTable((run) => run.per_k.recall, 'k')}
          legend={legend}
        />
        {sameCurveMetric ? (
          <ChartFigure
            headingLevel={3}
            title={`Training curve: ${labelA} vs ${labelB} (${a.training_curve_metric})`}
            description={`${a.training_curve_metric} by iteration for ${labelA} (solid) and ${labelB} (dashed).`}
            build={buildTraining}
            table={curveTable((run) => run.training_curve, 'Iteration')}
            legend={legend.map((item) => ({ ...item, hollow: false }))}
          />
        ) : (
          <p className="chart__note">
            {a.training_curve.length === 0 || b.training_curve.length === 0
              ? 'At least one of these runs has no training curve.'
              : `These runs log different training measures (${a.training_curve_metric} and ${b.training_curve_metric}), so their curves aren't overlaid.`}
          </p>
        )}
      </div>
    </>
  );
}

export function RunPicker({ runs }: { runs: RunReport[] }) {
  const byModel = comparableRuns(runs);
  const models = [...byModel.keys()];
  const [model, setModel] = useState(models[0] ?? '');
  const siblings = byModel.get(model) ?? [];
  const [picked, setPicked] = useState<[string, string]>([siblings[0]?.run_id ?? '', siblings[1]?.run_id ?? '']);
  const headingId = useId();
  const ids = { model: useId(), a: useId(), b: useId() };

  const labels = runLabels(siblings);
  const a = siblings.find((run) => run.run_id === picked[0]);
  const b = siblings.find((run) => run.run_id === picked[1]);

  function chooseModel(next: string) {
    const runsOfModel = byModel.get(next) ?? [];
    setModel(next);
    setPicked([runsOfModel[0]?.run_id ?? '', runsOfModel[1]?.run_id ?? '']);
  }

  const runSelect = (id: string, label: string, index: 0 | 1) => (
    <div className="field">
      <label htmlFor={id}>{label}</label>
      <select
        id={id}
        value={picked[index]}
        onChange={(event) => {
          const next: [string, string] = [...picked];
          next[index] = event.target.value;
          setPicked(next);
        }}
      >
        {siblings.map((run) => (
          <option key={run.run_id} value={run.run_id}>
            {labels.get(run.run_id)}
          </option>
        ))}
      </select>
    </div>
  );

  return (
    <section className="eval-section" aria-labelledby={headingId}>
      <h2 id={headingId}>Compare two runs</h2>
      {models.length === 0 ? (
        <p className="muted">
          Comparing needs at least two runs of the same model (for example, ALS with two different factor counts).
          Every model has a single run so far.
        </p>
      ) : (
        <>
          <p className="muted">Pick two runs of the same model to compare their metrics and curves.</p>
          <div className="run-picker">
            <div className="field">
              <label htmlFor={ids.model}>Model</label>
              <select id={ids.model} value={model} onChange={(event) => chooseModel(event.target.value)}>
                {models.map((m) => (
                  <option key={m} value={m}>
                    {m}
                  </option>
                ))}
              </select>
            </div>
            {runSelect(ids.a, 'Run A', 0)}
            {runSelect(ids.b, 'Run B', 1)}
          </div>
          {a && b && a.run_id === b.run_id && <p className="chart__note">Run A and run B are the same run; pick another.</p>}
          {a && b && (
            <Comparison a={a} b={b} labelA={labels.get(a.run_id) ?? a.run_id} labelB={labels.get(b.run_id) ?? b.run_id} />
          )}
        </>
      )}
    </section>
  );
}
