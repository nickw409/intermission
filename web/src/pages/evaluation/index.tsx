// Evaluation page: charts built from the offline run reports (GET /api/reports and
// GET /api/reports/{runId}), so visitors can see how the recommenders compare.
//
// Only schema_version 1 reports are charted. Anything else is listed under "Reports not shown"
// with the reason, never guessed at.
import { lazy, Suspense, useEffect, useId, useState } from 'react';
import { friendlyMessage } from '../../lib/errors';
import { loadReports, type LoadedReports, type SkippedRun } from '../../lib/reports/load';

// Observable Plot is large; only this page needs it.
import './evaluation.css';

const EvaluationCharts = lazy(() => import('./EvaluationCharts'));

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

      {state.status === 'ready' && state.data.runs.length > 0 && (
        <Suspense
          fallback={
            <p role="status" className="muted">
              Loading charts…
            </p>
          }
        >
          <EvaluationCharts runs={state.data.runs} />
        </Suspense>
      )}

      {state.status === 'ready' && <SkippedList skipped={state.data.skipped} />}
    </div>
  );
}
