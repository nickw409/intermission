// The chart part of the Evaluation page. Loaded on demand (see index.tsx) so Observable Plot and
// d3 stay out of the bundle every other page downloads.
import { useId, useMemo, useState } from 'react';
import type { RunReport } from '../../api/client';
import { groupRuns, latestPerModel } from '../../lib/reports/select';
import { ColdUsersChart, CoverageChart, NdcgChart, RecallChart, TrainingCurves } from './charts';
import { RunPicker } from './RunPicker';

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

export default function EvaluationCharts({ runs }: { runs: RunReport[] }) {
  const groups = useMemo(() => groupRuns(runs), [runs]);
  const [groupKey, setGroupKey] = useState(groups[0]?.key ?? '');
  const selectId = useId();
  const group = groups.find((g) => g.key === groupKey) ?? groups[0];
  if (!group) return null;
  return (
    <>
      {groups.length > 1 && (
        <div className="field eval-group-picker">
          <label htmlFor={selectId}>Dataset and split</label>
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
