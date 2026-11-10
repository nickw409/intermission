import { describe, expect, it } from 'vitest';
import { displayTitle, joinTitles, titleWithYear } from './format';

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
