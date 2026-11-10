import type { RunReport } from '../../api/client';

export class InvalidReport extends Error {}
export class UnsupportedReportVersion extends Error {
  readonly version: number = 0;
}

export function parseReport(_json: unknown): RunReport {
  throw new Error('NotImplemented');
}
