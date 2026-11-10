// Fetches the run-report listing and every report this site can display.
//
// Listing entries with an unknown schema_version (their model and headline are null) are never
// fetched. Each remaining report is fetched and parsed on its own, so one bad or missing report
// is reported next to the charts instead of breaking the page.
import { api, type RunReport, type Schemas } from '../../api/client';
import { friendlyMessage } from '../errors';
import { InvalidReport, parseReport, SUPPORTED_SCHEMA_VERSION, UnsupportedReportVersion } from './parse';

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

export function isSupportedSummary(item: ReportSummary): boolean {
  return item.schema_version === SUPPORTED_SCHEMA_VERSION && item.model !== null && item.headline !== null;
}

async function fetchReport(runId: string, signal: AbortSignal): Promise<RunReport | SkippedRun> {
  try {
    const body = await api.get('/api/reports/{runId}', { path: { runId }, signal });
    return parseReport(body);
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
  const skipped: SkippedRun[] = items
    .filter((item) => !isSupportedSummary(item))
    .map((item) => ({ run_id: item.run_id, reason: `unsupported report format (v${item.schema_version})` }));

  const results = await Promise.all(
    items.filter(isSupportedSummary).map((item) => fetchReport(item.run_id, signal)),
  );
  const runs: RunReport[] = [];
  for (const result of results) {
    if ('schema_version' in result) runs.push(result);
    else skipped.push(result);
  }
  runs.sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at));
  return { listed: items.length, runs, skipped };
}
