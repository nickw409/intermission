// Picks which runs go on which chart. Numbers from different datasets or splits are not
// comparable, so the page charts one (dataset, evaluated_on) group at a time.
import type { RunReport } from '../../api/client';
import { compareModels } from './models';

export interface RunGroup {
  /** "ml-32m · test" */
  key: string;
  /** Newest first. */
  runs: RunReport[];
}

export function groupKey(run: Pick<RunReport, 'dataset' | 'evaluated_on'>): string {
  return `${run.dataset} · ${run.evaluated_on}`;
}

/** Groups runs (given newest first); the group holding the newest run comes first. */
export function groupRuns(runs: RunReport[]): RunGroup[] {
  const groups = new Map<string, RunReport[]>();
  for (const run of runs) {
    const key = groupKey(run);
    groups.set(key, [...(groups.get(key) ?? []), run]);
  }
  return [...groups].map(([key, members]) => ({ key, runs: members }));
}

/** The newest run of each model (runs given newest first), in canonical model order. */
export function latestPerModel(runs: RunReport[]): RunReport[] {
  const latest = new Map<string, RunReport>();
  for (const run of runs) if (!latest.has(run.model)) latest.set(run.model, run);
  return [...latest.values()].sort((a, b) => compareModels(a.model, b.model));
}

/** Runs of each model that has at least two, keyed by model, in canonical model order. */
export function comparableRuns(runs: RunReport[]): Map<string, RunReport[]> {
  const byModel = new Map<string, RunReport[]>();
  for (const run of runs) byModel.set(run.model, [...(byModel.get(run.model) ?? []), run]);
  const models = [...byModel.keys()].filter((m) => byModel.get(m)!.length >= 2).sort(compareModels);
  return new Map(models.map((m) => [m, byModel.get(m)!]));
}
