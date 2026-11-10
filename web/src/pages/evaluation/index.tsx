// Evaluation page: charts built from the offline run reports (GET /api/reports and
// GET /api/reports/{runId}), so visitors can see how the recommenders compare.
//
// Only schema_version 1 reports are charted. Anything else is listed under "Reports not shown"
// with the reason, never guessed at.
import { useEffect, useId, useMemo, useState } from 'react';
import type { RunReport } from '../../api/client';
import { friendlyMessage } from '../../lib/errors';
import { loadReports, type LoadedReports, type SkippedRun } from '../../lib/reports/load';
import { groupRuns, latestPerModel } from '../../lib/reports/select';
import { ColdUsersChart, CoverageChart, NdcgChart, RecallChart, TrainingCurves } from './charts';
import { RunPicker } from './RunPicker';
import './evaluation.css';

type State = { status: 'loading' } | { status: 'error'; message: string } | { status: 'ready'; data: LoadedReports };

function SkippedList({ skipped }: { skipped: SkippedRun[] }) {
  const headingId = useId();
  if (skipped.length === 0) return null;
  return (
    <section className="eval-section eval-skipped" aria-labelledby={headingId}>
      <h2 id={headingId}>Reports not shown</h2>
      <ul>
        {skipped.map((run) => (
          <li key={run.run_id}>
            <code>{run.run_id}</code>: {run.reason}
          </li>
        ))}
      </ul>
    </section>
  );
}

function Overview({ runs }: { runs: RunReport[] }) {
  const latest = useMemo(() => latestPerModel(runs), [runs]);
  const headingId = useId();
  const replaced = runs.length - latest.length;
  return (
    <section className="eval-section" aria-labelledby={headingId}>
      <h2 id={headingId}>Model comparison</h2>
      <p className="muted">
        The newest run of each model.{' '}
        {replaced > 0 && `${replaced} older ${replaced === 1 ? 'run is' : 'runs are'} available in the run picker below.`}
      </p>
      <div className="chart-grid">
        <NdcgChart runs={latest} />
        <RecallChart runs={latest} />
        <CoverageChart runs={latest} />
        <ColdUsersChart runs={latest} />
      </div>
      <TrainingCurves runs={latest} />
    </section>
  );
}

function Charts({ runs }: { runs: RunReport[] }) {
  const groups = useMemo(() => groupRuns(runs), [runs]);
  const [groupKey, setGroupKey] = useState(groups[0]?.key ?? '');
  const selectId = useId();
  const group = groups.find((g) => g.key === groupKey) ?? groups[0];
  if (!group) return null;
  return (
    <>
      {groups.length > 1 && (
        <div className="field eval-group-picker">
          <label htmlFor={selectId}>Dataset</label>
          <select id={selectId} value={group.key} onChange={(event) => setGroupKey(event.target.value)}>
            {groups.map((g) => (
              <option key={g.key} value={g.key}>
                {g.key}
              </option>
            ))}
          </select>
          <p className="field__hint">Runs on different datasets or splits aren&apos;t comparable, so they are charted separately.</p>
        </div>
      )}
      <p className="muted">
        Showing {group.runs.length} {group.runs.length === 1 ? 'run' : 'runs'} on <strong>{group.key}</strong>.
      </p>
      <Overview key={`overview-${group.key}`} runs={group.runs} />
      <RunPicker key={`picker-${group.key}`} runs={group.runs} />
    </>
  );
}

export default function EvaluationPage() {
  const [state, setState] = useState<State>({ status: 'loading' });
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    const controller = new AbortController();
    loadReports(controller.signal)
      .then((data) => setState({ status: 'ready', data }))
      .catch((error: unknown) => {
        if (!controller.signal.aborted) setState({ status: 'error', message: friendlyMessage(error) });
      });
    return () => controller.abort();
  }, [attempt]);

  function retry() {
    setState({ status: 'loading' });
    setAttempt((n) => n + 1);
  }

  return (
    <div className="evaluation">
      <h1>Evaluation</h1>
      <p className="evaluation__intro">
        How well each recommender ranks movies people went on to rate highly (4 stars or more), measured offline on
        held-out ratings. Intervals are 95% bootstrap confidence intervals. Every model must beat the popularity baseline
        to count as a success.
      </p>

      {state.status === 'loading' && (
        <p role="status" className="muted">
          Loading evaluation runs…
        </p>
      )}

      {state.status === 'error' && (
        <div className="form-error eval-error" role="alert">
          <p>{state.message}</p>
          <button type="button" className="button" onClick={retry}>
            Try again
          </button>
        </div>
      )}

      {state.status === 'ready' && state.data.listed === 0 && (
        <p className="muted">No evaluation runs yet. Charts appear here once the first model has been evaluated.</p>
      )}

      {state.status === 'ready' && state.data.listed > 0 && state.data.runs.length === 0 && (
        <p className="muted">No evaluation runs this site can display yet.</p>
      )}

      {state.status === 'ready' && state.data.runs.length > 0 && <Charts runs={state.data.runs} />}

      {state.status === 'ready' && <SkippedList skipped={state.data.skipped} />}
    </div>
  );
}
