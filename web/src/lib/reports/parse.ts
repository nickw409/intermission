// Turns run-report JSON (contracts/report.schema.json) into a typed RunReport, or refuses it.
//
// The schema's rule is that `schema_version` is the major version and the UI must reject any
// value it does not know, so the version is checked before anything else: a v2 report is
// "unsupported", never "invalid", even if v2 renamed every other field.
//
// The structural checks below mirror report.schema.json by hand. A generic JSON Schema validator
// (e.g. Ajv) compiles validators with `new Function`, which the site's Content-Security-Policy
// forbids, and the schema is small and frozen. parse.test.ts runs every fixture through this
// and breaks one rule at a time, so drift from the contract shows up as a failing test.
import type { RunReport } from '../../api/client';

export const SUPPORTED_SCHEMA_VERSION = 1;

/** The JSON is not a well-formed v1 report. `path` names the first offending field. */
export class InvalidReport extends Error {
  readonly path: string;

  constructor(path: string, problem: string) {
    super(`Invalid report: ${path || 'the report'} ${problem}.`);
    this.name = 'InvalidReport';
    this.path = path;
  }
}

/** The report declares a schema_version this site does not know how to display. */
export class UnsupportedReportVersion extends Error {
  readonly version: number;

  constructor(version: number) {
    super(unsupportedVersionMessage(version));
    this.name = 'UnsupportedReportVersion';
    this.version = version;
  }
}

export function unsupportedVersionMessage(version: number): string {
  return version > SUPPORTED_SCHEMA_VERSION
    ? `This report uses a newer format (v${version}) that this site can't display.`
    : `This report uses an unknown format (v${version}) that this site can't display.`;
}

/**
 * Parses a run report. Accepts a JSON string or an already-parsed value.
 * Throws UnsupportedReportVersion for any schema_version other than 1, and InvalidReport when the
 * value does not match the v1 schema.
 */
export function parseReport(json: unknown): RunReport {
  let value = json;
  if (typeof json === 'string') {
    try {
      value = JSON.parse(json);
    } catch {
      throw new InvalidReport('', 'is not valid JSON');
    }
  }
  const root = object(value, '');
  const version = root.schema_version;
  if (!Number.isInteger(version)) throw new InvalidReport('schema_version', 'must be an integer');
  if (version !== SUPPORTED_SCHEMA_VERSION) throw new UnsupportedReportVersion(version as number);
  checkReport(root);
  return root as unknown as RunReport;
}

// ---- Checks, one per schema construct ----

type Json = Record<string, unknown>;

/** contracts/public.yaml and report.schema.json: the only run ids that may appear in a URL. */
export const RUN_ID = /^[A-Za-z0-9_-]{1,100}$/;
// RFC 3339 date-time (the schema's "format": "date-time"): a time zone, Z or ±hh:mm, is required.
const RFC3339 = /^\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}(\.\d+)?([Zz]|[+-]\d{2}:\d{2})$/;
const SHA256_HEX = /^[0-9a-f]{64}$/;

function join(path: string, key: string | number): string {
  if (typeof key === 'number') return `${path}[${key}]`;
  return path ? `${path}.${key}` : key;
}

function object(value: unknown, path: string): Json {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new InvalidReport(path, 'must be an object');
  }
  return value as Json;
}

/** Requires exactly these keys (the schema uses additionalProperties: false throughout). */
function exactKeys(obj: Json, path: string, keys: readonly string[]): void {
  for (const key of keys) {
    if (!Object.hasOwn(obj, key)) throw new InvalidReport(join(path, key), 'is missing');
  }
  for (const key of Object.keys(obj)) {
    if (!keys.includes(key)) throw new InvalidReport(join(path, key), 'is not a known field');
  }
}

function number(value: unknown, path: string, min = -Infinity, max = Infinity): void {
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new InvalidReport(path, 'must be a number');
  if (value < min || value > max) throw new InvalidReport(path, `must be between ${min} and ${max}`);
}

function integer(value: unknown, path: string, min = -Infinity, max = Infinity): void {
  if (!Number.isInteger(value)) throw new InvalidReport(path, 'must be an integer');
  number(value, path, min, max);
}

function string(value: unknown, path: string, pattern?: RegExp): void {
  if (typeof value !== 'string') throw new InvalidReport(path, 'must be a string');
  if (pattern && !pattern.test(value)) throw new InvalidReport(path, `must match ${pattern.source}`);
}

function dateTime(value: unknown, path: string): void {
  string(value, path);
  if (!RFC3339.test(value as string) || Number.isNaN(Date.parse(value as string))) {
    throw new InvalidReport(path, 'must be an RFC 3339 date-time');
  }
}

function estimate(value: unknown, path: string): void {
  const obj = object(value, path);
  exactKeys(obj, path, ['mean', 'ci95']);
  number(obj.mean, join(path, 'mean'));
  const ci = obj.ci95;
  if (!Array.isArray(ci) || ci.length !== 2) throw new InvalidReport(join(path, 'ci95'), 'must be [low, high]');
  ci.forEach((v, i) => number(v, join(join(path, 'ci95'), i)));
}

function curve(value: unknown, path: string): void {
  if (!Array.isArray(value)) throw new InvalidReport(path, 'must be an array of [x, y] points');
  value.forEach((point, i) => {
    const p = join(path, i);
    if (!Array.isArray(point) || point.length !== 2) throw new InvalidReport(p, 'must be an [x, y] pair');
    integer(point[0], join(p, 0));
    number(point[1], join(p, 1));
  });
}

function ratingErrors(value: unknown, path: string): void {
  const obj = object(value, path);
  exactKeys(obj, path, ['rmse', 'mae', 'n']);
  estimate(obj.rmse, join(path, 'rmse'));
  estimate(obj.mae, join(path, 'mae'));
  integer(obj.n, join(path, 'n'), 0);
}

function nullOr(value: unknown, path: string, check: (v: unknown, p: string) => void): void {
  if (value !== null) check(value, path);
}

const TOP_LEVEL = [
  'schema_version',
  'run_id',
  'created_at',
  'git_sha',
  'dataset',
  'split_hash',
  'split',
  'evaluated_on',
  'model',
  'config',
  'seeds',
  'train_seconds',
  'eval_seconds',
  'peak_rss_bytes',
  'counts',
  'cold_item_share',
  'metrics',
  'aggregates',
  'per_k',
  'slices',
  'rating_metrics',
  'rating_baselines',
  'training_curve',
  'training_curve_metric',
  'foldin_jaccard@10',
] as const;

const METRICS = ['ndcg@10', 'recall@10', 'recall@50', 'precision@10', 'map@10', 'mean_pop_rank@10'] as const;

function checkReport(r: Json): void {
  exactKeys(r, '', TOP_LEVEL);
  string(r.run_id, 'run_id', RUN_ID);
  dateTime(r.created_at, 'created_at');
  string(r.git_sha, 'git_sha');
  string(r.dataset, 'dataset');
  string(r.split_hash, 'split_hash', SHA256_HEX);

  const split = object(r.split, 'split');
  exactKeys(split, 'split', ['min_ratings', 'test_fraction', 'val_fraction', 'test_min', 'test_max', 'seed']);
  integer(split.min_ratings, 'split.min_ratings');
  number(split.test_fraction, 'split.test_fraction');
  number(split.val_fraction, 'split.val_fraction');
  integer(split.test_min, 'split.test_min');
  integer(split.test_max, 'split.test_max');
  integer(split.seed, 'split.seed');

  if (r.evaluated_on !== 'test' && r.evaluated_on !== 'validation') {
    throw new InvalidReport('evaluated_on', 'must be "test" or "validation"');
  }
  string(r.model, 'model');
  object(r.config, 'config');

  const seeds = object(r.seeds, 'seeds');
  for (const [key, seed] of Object.entries(seeds)) integer(seed, join('seeds', key), 0, Number.MAX_SAFE_INTEGER);

  number(r.train_seconds, 'train_seconds', 0);
  number(r.eval_seconds, 'eval_seconds', 0);
  nullOr(r.peak_rss_bytes, 'peak_rss_bytes', (v, p) => integer(v, p, 0));

  const counts = object(r.counts, 'counts');
  const countKeys = ['users_evaluated', 'users_without_relevant', 'n_users', 'n_items', 'n_train_ratings'];
  exactKeys(counts, 'counts', countKeys);
  for (const key of countKeys) integer(counts[key], join('counts', key), 0);

  number(r.cold_item_share, 'cold_item_share', 0, 1);

  const metrics = object(r.metrics, 'metrics');
  exactKeys(metrics, 'metrics', METRICS);
  for (const key of METRICS) estimate(metrics[key], join('metrics', key));

  const aggregates = object(r.aggregates, 'aggregates');
  exactKeys(aggregates, 'aggregates', ['coverage@10']);
  number(aggregates['coverage@10'], 'aggregates.coverage@10', 0, 1);

  const perK = object(r.per_k, 'per_k');
  exactKeys(perK, 'per_k', ['ndcg', 'recall', 'precision']);
  for (const key of ['ndcg', 'recall', 'precision']) curve(perK[key], join('per_k', key));

  const slices = object(r.slices, 'slices');
  exactKeys(slices, 'slices', ['cold_users']);
  const cold = object(slices.cold_users, 'slices.cold_users');
  exactKeys(cold, 'slices.cold_users', ['train_ratings_lt', 'users', 'ndcg@10']);
  integer(cold.train_ratings_lt, 'slices.cold_users.train_ratings_lt');
  integer(cold.users, 'slices.cold_users.users', 0);
  nullOr(cold['ndcg@10'], 'slices.cold_users.ndcg@10', estimate);

  nullOr(r.rating_metrics, 'rating_metrics', ratingErrors);
  nullOr(r.rating_baselines, 'rating_baselines', (v, p) => {
    const obj = object(v, p);
    const keys = ['global_mean', 'user_bias', 'item_bias', 'user_item_bias'];
    exactKeys(obj, p, keys);
    for (const key of keys) ratingErrors(obj[key], join(p, key));
  });

  curve(r.training_curve, 'training_curve');
  string(r.training_curve_metric, 'training_curve_metric');
  nullOr(r['foldin_jaccard@10'], 'foldin_jaccard@10', (v, p) => number(v, p, 0, 1));
}
