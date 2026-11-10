import { describe, expect, it } from 'vitest';
import { posterUrl } from './poster';

describe('posterUrl', () => {
  it('is a same-origin path built only from a positive integer id', () => {
    expect(posterUrl(4993)).toBe('/api/posters/4993');
  });

  it.each([0, -1, 1.5, Number.NaN, 2147483648])('refuses %s', (id) => {
    expect(() => posterUrl(id)).toThrow();
  });
});
