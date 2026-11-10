import { describe, expect, it } from 'vitest';
import { compareModels, configLabel, modelStyle, popularityVerdict, runLabels } from './models';

describe('modelStyle', () => {
  it('gives each known model its own fixed color, symbol and dash, independent of what else is shown', () => {
    const models = ['pop', 'pop_count', 'knn', 'content', 'mf', 'als'];
    const styles = models.map(modelStyle);
    expect(new Set(styles.map((s) => s.light)).size).toBe(6);
    expect(new Set(styles.map((s) => s.dark)).size).toBe(6);
    expect(new Set(styles.map((s) => s.symbol)).size).toBe(6);
    expect(new Set(styles.map((s) => s.dash)).size).toBe(6);
    // Color follows the entity, never its position in a filtered list.
    expect(modelStyle('als')).toEqual(modelStyle('als'));
    expect(modelStyle('als').light).toBe(styles[5]?.light);
  });

  it('still styles an unknown model without reusing a known model color', () => {
    const known = new Set(['pop', 'pop_count', 'knn', 'content', 'mf', 'als'].map((m) => modelStyle(m).light));
    expect(known.has(modelStyle('hybrid').light)).toBe(false);
  });
});

describe('compareModels', () => {
  it('orders models baseline-first in the canonical order, unknown ones last alphabetically', () => {
    expect(['als', 'zeta', 'pop', 'knn', 'alpha', 'pop_count'].sort(compareModels)).toEqual([
      'pop',
      'pop_count',
      'knn',
      'als',
      'alpha',
      'zeta',
    ]);
  });
});

describe('configLabel / runLabels', () => {
  it('formats a config as key=value pairs', () => {
    expect(configLabel({ k: 128, lambda: 0.05 })).toBe('k=128, lambda=0.05');
    expect(configLabel({})).toBe('default config');
  });

  it('labels sibling runs of one model by the config keys that differ', () => {
    const labels = runLabels([
      { run_id: 'a', model: 'als', config: { alpha: 40, iters: 15, k: 128, lambda: 0.05 }, created_at: '2026-11-09T22:02:10Z' },
      { run_id: 'b', model: 'als', config: { alpha: 40, iters: 15, k: 64, lambda: 0.05 }, created_at: '2026-11-09T21:14:03Z' },
    ]);
    expect(labels.get('a')).toBe('als · k=128');
    expect(labels.get('b')).toBe('als · k=64');
  });

  it('falls back to the run time when configs are identical', () => {
    const labels = runLabels([
      { run_id: 'a', model: 'knn', config: { neighbors: 100 }, created_at: '2026-11-09T22:02:10Z' },
      { run_id: 'b', model: 'knn', config: { neighbors: 100 }, created_at: '2026-11-08T21:14:03Z' },
    ]);
    expect(labels.get('a')).toBe('knn · neighbors=100 · run 2026-11-09 22:02 UTC');
    expect(labels.get('b')).toBe('knn · neighbors=100 · run 2026-11-08 21:14 UTC');
  });
});

describe('popularityVerdict (CI-overlap rule)', () => {
  const pop = { mean: 0.0412, ci95: [0.0403, 0.0421] as [number, number] };

  it('beats popularity only when the whole CI is above popularity\'s CI', () => {
    expect(popularityVerdict({ mean: 0.09, ci95: [0.0893, 0.0911] }, pop)).toBe('beats');
    // pop_count fixture: lower bound 0.0427 > 0.0421.
    expect(popularityVerdict({ mean: 0.0436, ci95: [0.0427, 0.0445] }, pop)).toBe('beats');
  });

  it('fails when the CI overlaps popularity\'s, even if the mean is higher', () => {
    expect(popularityVerdict({ mean: 0.043, ci95: [0.0421, 0.044] }, pop)).toBe('fails');
    expect(popularityVerdict({ mean: 0.0415, ci95: [0.041, 0.042] }, pop)).toBe('fails');
  });

  it('fails when the CI is entirely below popularity\'s', () => {
    expect(popularityVerdict({ mean: 0.0198, ci95: [0.0189, 0.0207] }, pop)).toBe('fails');
  });
});
