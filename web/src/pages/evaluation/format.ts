// Number formatting shared by charts, tooltips and data tables, so a value reads the same everywhere.

/** Ranking metrics and coverage: 4 decimal places (e.g. 0.0902). */
export function metric(value: number): string {
  return value.toFixed(4);
}

/** A 95% confidence interval: "0.0893 – 0.0911". */
export function interval(ci95: readonly number[], digits = 4): string {
  return `${(ci95[0] ?? NaN).toFixed(digits)} – ${(ci95[1] ?? NaN).toFixed(digits)}`;
}

const plain = new Intl.NumberFormat('en-US', { maximumFractionDigits: 4 });

/** Any other number (training losses, counts): grouped, up to 4 decimals. */
export function number(value: number): string {
  return plain.format(value);
}

export const MISSING = '—';
