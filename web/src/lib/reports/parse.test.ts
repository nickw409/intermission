/* eslint-disable @typescript-eslint/no-explicit-any -- the tests mutate fixture JSON freely */
import { describe, expect, it } from 'vitest';
import { InvalidReport, parseReport, UnsupportedReportVersion } from './parse';

const fixtureFiles = import.meta.glob<unknown>('../../../../contracts/fixtures/reports/*.json', {
  eager: true,
  import: 'default',
});

function fixture(name: string): Record<string, unknown> {
  const value = fixtureFiles[`../../../../contracts/fixtures/reports/${name}.json`];
  if (value === undefined) throw new Error(`missing fixture ${name}`);
  return structuredClone(value) as Record<string, unknown>;
}

const V1_FIXTURES = ['pop', 'pop_count', 'knn', 'content', 'mf', 'als', 'als_k128'];

describe('parseReport', () => {
  it.each(V1_FIXTURES)('accepts the schema_version 1 fixture %s', (name) => {
    const report = parseReport(fixture(name));
    expect(report.schema_version).toBe(1);
    expect(report.model).toBe(fixture(name).model);
  });

  it('accepts a JSON string as well as a parsed value', () => {
    expect(parseReport(JSON.stringify(fixture('als'))).run_id).toBe('2026-11-09T21-14-03Z_als');
  });

  it('throws UnsupportedReportVersion for the unknown_major fixture instead of guessing', () => {
    let caught: unknown;
    try {
      parseReport(fixture('unknown_major'));
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(UnsupportedReportVersion);
    expect((caught as UnsupportedReportVersion).version).toBe(2);
    expect((caught as Error).message).toBe("This report uses a newer format (v2) that this site can't display.");
  });

  it.each([0, 3, 17])('throws UnsupportedReportVersion for schema_version %i even if the body looks valid', (v) => {
    const body = fixture('als');
    body.schema_version = v;
    expect(() => parseReport(body)).toThrow(UnsupportedReportVersion);
  });

  it('checks the version before the structure, so a v2 report with new fields is still "unsupported"', () => {
    expect(() => parseReport({ schema_version: 2, brand_new_field: true })).toThrow(UnsupportedReportVersion);
  });

  it.each([
    ['not JSON', '{nope', /not valid JSON/],
    ['an array', [], /must be an object/],
    ['null', null, /must be an object/],
    ['missing schema_version', { run_id: 'x' }, /schema_version/],
    ['a string schema_version', { schema_version: '1' }, /schema_version/],
    ['a fractional schema_version', { schema_version: 1.5 }, /schema_version/],
  ])('rejects %s with InvalidReport', (_label, input, message) => {
    expect(() => parseReport(input)).toThrow(InvalidReport);
    expect(() => parseReport(input)).toThrow(message);
  });

  // Each mutation breaks one rule of contracts/report.schema.json; the error names the field.
  it.each<[string, (r: Record<string, any>) => void, string]>([
    ['a missing required field', (r) => delete r.metrics, 'metrics'],
    ['an extra top-level field', (r) => (r.surprise = 1), 'surprise'],
    ['a bad run_id', (r) => (r.run_id = '../etc/passwd'), 'run_id'],
    ['a bad split_hash', (r) => (r.split_hash = 'abc'), 'split_hash'],
    ['a bad evaluated_on', (r) => (r.evaluated_on = 'train'), 'evaluated_on'],
    ['a non-number metric mean', (r) => (r.metrics['ndcg@10'].mean = '0.1'), 'metrics.ndcg@10.mean'],
    ['a one-sided CI', (r) => (r.metrics['recall@50'].ci95 = [0.1]), 'metrics.recall@50.ci95'],
    ['a missing metric', (r) => delete r.metrics['map@10'], 'metrics.map@10'],
    ['coverage above 1', (r) => (r.aggregates['coverage@10'] = 1.5), 'aggregates.coverage@10'],
    ['a curve point with three entries', (r) => (r.per_k.recall[0] = [1, 2, 3]), 'per_k.recall[0]'],
    ['a fractional curve x', (r) => (r.training_curve[0] = [1.5, 2]), 'training_curve[0][0]'],
    ['a missing cold-user slice', (r) => delete r.slices.cold_users, 'slices.cold_users'],
    ['a string seed', (r) => (r.seeds.model = '42'), 'seeds.model'],
    ['a negative count', (r) => (r.counts.users_evaluated = -1), 'counts.users_evaluated'],
    ['a non-object config', (r) => (r.config = 'k=64'), 'config'],
    ['a bad training_curve_metric', (r) => (r.training_curve_metric = 3), 'training_curve_metric'],
    ['an out-of-range fold-in Jaccard', (r) => (r['foldin_jaccard@10'] = 2), 'foldin_jaccard@10'],
    ['malformed rating metrics', (r) => (r.rating_metrics = { rmse: 1 }), 'rating_metrics'],
    ['a created_at without a time zone', (r) => (r.created_at = '2026-11-09T22:02:10'), 'created_at'],
    ['a date-only created_at', (r) => (r.created_at = '2026-11-09'), 'created_at'],
    ['a free-form created_at', (r) => (r.created_at = 'Mon, 09 Nov 2026 22:02:10 GMT'), 'created_at'],
    ['a negative peak_rss_bytes', (r) => (r.peak_rss_bytes = -1), 'peak_rss_bytes'],
    ['a fractional peak_rss_bytes', (r) => (r.peak_rss_bytes = 1.5), 'peak_rss_bytes'],
    ['a missing split field', (r) => delete r.split.test_fraction, 'split.test_fraction'],
    ['a string split seed', (r) => (r.split.seed = '42'), 'split.seed'],
    ['a fractional split.min_ratings', (r) => (r.split.min_ratings = 10.5), 'split.min_ratings'],
    ['an extra split field', (r) => (r.split.extra = 1), 'split.extra'],
    ['a seed above MAX_SAFE_INTEGER', (r) => (r.seeds.model = Number.MAX_SAFE_INTEGER + 2), 'seeds.model'],
    ['a negative seed', (r) => (r.seeds.model = -1), 'seeds.model'],
    ['cold_item_share above 1', (r) => (r.cold_item_share = 1.01), 'cold_item_share'],
    ['a negative cold_item_share', (r) => (r.cold_item_share = -0.1), 'cold_item_share'],
    ['rating_baselines missing a baseline', (r) => (r.rating_baselines = {}), 'rating_baselines.global_mean'],
    ['a non-object rating_baselines', (r) => (r.rating_baselines = 3), 'rating_baselines'],
  ])('rejects %s and names the field', (_label, mutate, path) => {
    const body = fixture('mf');
    mutate(body);
    expect(() => parseReport(body)).toThrow(InvalidReport);
    expect(() => parseReport(body)).toThrow(path);
  });

  it.each(['2026-11-09T22:02:10Z', '2026-11-09T22:02:10.123Z', '2026-11-09T22:02:10+01:00', '2026-11-09t22:02:10z'])(
    'accepts the RFC 3339 created_at %s',
    (createdAt) => {
      const body = fixture('mf');
      body.created_at = createdAt;
      expect(parseReport(body).created_at).toBe(createdAt);
    },
  );

  it('accepts a null cold-user NDCG (slice with no eligible users)', () => {
    const body = fixture('knn') as Record<string, any>;
    body.slices.cold_users['ndcg@10'] = null;
    expect(parseReport(body).slices.cold_users['ndcg@10']).toBeNull();
  });

  it('accepts rating metrics and baselines when present', () => {
    const body = fixture('mf') as Record<string, any>;
    const errors = { rmse: { mean: 0.8, ci95: [0.79, 0.81] }, mae: { mean: 0.6, ci95: [0.59, 0.61] }, n: 10 };
    body.rating_metrics = errors;
    body.rating_baselines = { global_mean: errors, user_bias: errors, item_bias: errors, user_item_bias: errors };
    expect(parseReport(body).rating_metrics?.n).toBe(10);
  });
});
