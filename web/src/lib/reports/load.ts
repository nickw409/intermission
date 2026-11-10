// Fetches the run-report listing and every report this site can display.
//
// A listing entry is never fetched when its run_id isn't a valid id (so no odd path ever reaches
// a URL), when it repeats an earlier entry, when its schema_version is unknown, or when its
// summary is missing. Each remaining report is fetched and parsed on its own, so one bad or
// missing report is reported next to the charts instead of breaking the page.
import { api, type RunReport, type Schemas } from '../../api/client';
import { friendlyMessage } from '../errors';
import { InvalidReport, parseReport, RUN_ID, SUPPORTED_SCHEMA_VERSION, UnsupportedReportVersion } from './parse';

export type ReportSummary = Schemas['ReportSummary'];

/** A run that is not charted, and why. */
export interface SkippedRun {
  run_id: string;
  reason: string;
}

export interface LoadedReports {
  /** Number of entries in the listing. */
  listed: number;
  /** Parsed v1 reports, newest first. */
  runs: RunReport[];
  skipped: SkippedRun[];
}

/** Why a listing entry can't be charted, or null if its report should be fetched. */
export function summaryProblem(item: ReportSummary): string | null {
  if (!RUN_ID.test(item.run_id)) return 'not a valid run id, so it was not loaded';
  if (item.schema_version !== SUPPORTED_SCHEMA_VERSION) return `unsupported report format (v${item.schema_version})`;
  if (item.model === null || item.headline === null) {
    return 'the listing has no model or headline metrics for this run, so it was not loaded';
  }
  return null;
}

async function fetchReport(runId: string, signal: AbortSignal): Promise<RunReport | SkippedRun> {
  try {
    const body = await api.get('/api/reports/{runId}', { path: { runId }, signal });
    const report = parseReport(body);
    if (report.run_id !== runId) {
      return { run_id: runId, reason: `the server returned run ${report.run_id} instead, so it was not shown` };
    }
    return report;
  } catch (error) {
    if (signal.aborted) throw error;
    if (error instanceof UnsupportedReportVersion || error instanceof InvalidReport) {
      return { run_id: runId, reason: error.message };
    }
    return { run_id: runId, reason: friendlyMessage(error) };
  }
}

/** Rejects only if the listing itself can't be loaded. */
export async function loadReports(signal: AbortSignal): Promise<LoadedReports> {
  const { items } = await api.get('/api/reports', { signal });
  const skipped: SkippedRun[] = [];
  const toFetch: string[] = [];
  const seen = new Set<string>();
  for (const item of items) {
    if (seen.has(item.run_id)) {
      skipped.push({ run_id: item.run_id, reason: 'listed more than once; shown once' });
      continue;
    }
    seen.add(item.run_id);
    const problem = summaryProblem(item);
    if (problem) skipped.push({ run_id: item.run_id, reason: problem });
    else toFetch.push(item.run_id);
  }

  const results = await Promise.all(toFetch.map((runId) => fetchReport(runId, signal)));
  const runs: RunReport[] = [];
  for (const result of results) {
    if ('schema_version' in result) runs.push(result);
    else skipped.push(result);
  }
  runs.sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at));
  return { listed: items.length, runs, skipped };
}
