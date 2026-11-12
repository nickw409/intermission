import { describe, expect, it, vi } from 'vitest';
import { displayTitle, formatReleaseDate, formatRuntime, joinTitles, titleWithYear } from './format';

describe('displayTitle', () => {
  it.each([
    ['Matrix, The', 'The Matrix'],
    ['Lord of the Rings: The Fellowship of the Ring, The', 'The Lord of the Rings: The Fellowship of the Ring'],
    ['Beautiful Mind, A', 'A Beautiful Mind'],
    ['American Tail, An', 'An American Tail'],
    ['Toy Story', 'Toy Story'],
    ['Good, the Bad and the Ugly, The', 'The Good, the Bad and the Ugly'],
    ['Thereafter, Then', 'Thereafter, Then'],
  ])('%s -> %s', (raw, shown) => {
    expect(displayTitle(raw)).toBe(shown);
  });
});

describe('titleWithYear', () => {
  it('appends the year in parentheses, or nothing when unknown', () => {
    expect(titleWithYear('Matrix, The', 1999)).toBe('The Matrix (1999)');
    expect(titleWithYear('Toy Story', null)).toBe('Toy Story');
  });
});

describe('joinTitles', () => {
  it('joins with commas and a final "and"', () => {
    expect(joinTitles([])).toBe('');
    expect(joinTitles(['A'])).toBe('A');
    expect(joinTitles(['A', 'B'])).toBe('A and B');
    expect(joinTitles(['A', 'B', 'C'])).toBe('A, B and C');
  });
});

describe('formatRuntime', () => {
  it.each([
    [179, '2 h 59 min'],
    [120, '2 h'],
    [45, '45 min'],
    [61, '1 h 1 min'],
  ])('%d -> %s', (minutes, shown) => {
    expect(formatRuntime(minutes)).toBe(shown);
  });
});

describe('formatReleaseDate', () => {
  it('formats the calendar day without shifting it across time zones', () => {
    const shown = formatReleaseDate('2001-12-18');
    expect(shown).toContain('2001');
    expect(shown).toContain('18');
    expect(shown).not.toContain('17');
    expect(formatReleaseDate('2001-01-01')).toContain('2001');
  });

  it.each(['America/Los_Angeles', 'Pacific/Kiritimati'])('keeps the calendar day in %s', async (zone) => {
    // The formatter is built at import, so import a fresh copy under the zone.
    vi.stubEnv('TZ', zone);
    vi.resetModules();
    try {
      const { formatReleaseDate: format } = await import('./format');
      expect(format('2001-12-18')).toMatch(/\b18\b/);
      expect(format('2001-01-01')).toMatch(/\b1\b/);
      expect(format('2001-01-01')).toContain('2001');
      expect(format('2001-12-31')).toContain('2001');
    } finally {
      vi.unstubAllEnvs();
      vi.resetModules();
    }
  });

  it('returns anything that is not YYYY-MM-DD unchanged', () => {
    expect(formatReleaseDate('soon')).toBe('soon');
  });
});
