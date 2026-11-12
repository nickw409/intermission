import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  cleanYears,
  filterQuery,
  fromSearchParams,
  hasYears,
  parseFilter,
  parseYear,
  readStored,
  releasedPhrase,
  STORAGE_KEY,
  toSearchParams,
  writeStored,
  yearPhrase,
  type YearRange,
} from './choice';

const NO_YEARS: YearRange = { from: null, to: null };
const NINETIES: YearRange = { from: 1990, to: 1999 };

describe('parseYear', () => {
  it.each([
    ['1990', 1990],
    ['1800', 1800],
    ['2100', 2100],
    [1995, 1995],
  ])('accepts %j', (value, year) => {
    expect(parseYear(value)).toBe(year);
  });

  it.each([
    ['empty', ''],
    ['three digits', '199'],
    ['five digits', '19900'],
    ['before 1800', '1799'],
    ['after 2100', '2101'],
    ['a sign', '+1990'],
    ['a negative number', '-1990'],
    ['a decimal', '1990.0'],
    ['an exponent', '2e3'],
    ['spaces', ' 1990'],
    ['words', 'nineties'],
    ['a fraction', 1990.5],
    ['an out-of-range number', 1700],
    ['null', null],
    ['an array', ['1990']],
  ])('rejects %s', (_, value) => {
    expect(parseYear(value)).toBeNull();
  });
});

describe('cleanYears', () => {
  it('keeps a range, one-sided bounds and a single year', () => {
    expect(cleanYears(1990, 1999)).toEqual(NINETIES);
    expect(cleanYears(1990, null)).toEqual({ from: 1990, to: null });
    expect(cleanYears(null, 1999)).toEqual({ from: null, to: 1999 });
    expect(cleanYears(1995, 1995)).toEqual({ from: 1995, to: 1995 });
  });

  it('drops both bounds when from is later than to (an inverted range is not guessed at)', () => {
    expect(cleanYears(2000, 1990)).toEqual(NO_YEARS);
  });

  it('treats an absent or empty bound as unbounded, but drops both bounds when one is not a year', () => {
    expect(cleanYears(1990, undefined)).toEqual({ from: 1990, to: null });
    expect(cleanYears('', '1999')).toEqual({ from: null, to: 1999 });
    expect(cleanYears(1990, 'abc')).toEqual(NO_YEARS);
    expect(cleanYears(1700, 1999)).toEqual(NO_YEARS);
    expect(cleanYears('1990', 2200)).toEqual(NO_YEARS);
  });
});

describe('hasYears', () => {
  it('is true when either bound is set', () => {
    expect(hasYears(NO_YEARS)).toBe(false);
    expect(hasYears({ from: 1990, to: null })).toBe(true);
    expect(hasYears({ from: null, to: 1999 })).toBe(true);
  });
});

describe('year phrases', () => {
  it.each([
    [NO_YEARS, '', ''],
    [NINETIES, '1990–1999', 'released 1990–1999'],
    [{ from: 1995, to: 1995 }, '1995', 'released in 1995'],
    [{ from: 1990, to: null }, 'from 1990', 'released 1990 or later'],
    [{ from: null, to: 1999 }, 'up to 1999', 'released 1999 or earlier'],
  ])('%j -> %j / %j', (years, short, sentence) => {
    expect(yearPhrase(years)).toBe(short);
    expect(releasedPhrase(years)).toBe(sentence);
  });
});

describe('URL', () => {
  it('reads the years with the model and genres', () => {
    expect(fromSearchParams(new URLSearchParams('model=knn&genres=Horror&year_from=1990&year_to=1999'))).toEqual({
      model: 'knn',
      genres: ['Horror'],
      years: NINETIES,
    });
  });

  it.each([['year_from=1990'], ['year_to=1999'], ['year_from='], ['year_to=bogus']])(
    'treats a URL with only %s as the whole view',
    (query) => {
      const raw = fromSearchParams(new URLSearchParams(query));
      expect(raw).not.toBeNull();
      expect(raw?.model).toBeNull();
      expect(raw?.genres).toEqual([]);
    },
  );

  it('is null without any view key', () => {
    expect(fromSearchParams(new URLSearchParams('other=1'))).toBeNull();
  });

  it.each([
    ['year_from=199&year_to=1999', NO_YEARS],
    ['year_from=1990&year_to=2200', NO_YEARS],
    ['year_from=1990&year_to=abc', NO_YEARS],
    ['year_from=abc&year_to=', NO_YEARS],
    ['year_from=1990&year_to=', { from: 1990, to: null }],
    ['year_from=2000&year_to=1990', NO_YEARS],
    ['year_from=1990&year_from=1999', { from: 1990, to: null }],
  ])('drops invalid years in %s', (query, years) => {
    expect(fromSearchParams(new URLSearchParams(query))?.years).toEqual(years);
  });

  it('writes the years only when set, after the model and genres', () => {
    expect(toSearchParams({ model: 'als', genres: ['Horror'], years: NINETIES }, true).toString()).toBe(
      'model=als&genres=Horror&year_from=1990&year_to=1999',
    );
    expect(toSearchParams({ model: 'als', genres: [], years: { from: null, to: 1999 } }, false).toString()).toBe(
      'year_to=1999',
    );
    expect(toSearchParams({ model: 'als', genres: [], years: NO_YEARS }, false).toString()).toBe('');
  });

  it('round-trips a view', () => {
    const choice = { model: 'knn' as const, genres: ['Action' as const, 'Horror' as const], years: NINETIES };
    expect(fromSearchParams(toSearchParams(choice, true))).toEqual(choice);
  });
});

describe('filter', () => {
  it('reads the genres and years from a query string, ignoring the model', () => {
    expect(parseFilter(new URLSearchParams('model=knn&genres=Horror&year_to=1999'))).toEqual({
      genres: ['Horror'],
      years: { from: null, to: 1999 },
    });
  });

  it('gives the /api/recommend query with only the parts that are set', () => {
    expect(filterQuery({ genres: [], years: NO_YEARS })).toEqual({});
    expect(filterQuery({ genres: ['Horror', 'Action'], years: NINETIES })).toEqual({
      genres: 'Action,Horror',
      year_from: 1990,
      year_to: 1999,
    });
    expect(filterQuery({ genres: [], years: { from: 1990, to: null } })).toEqual({ year_from: 1990 });
  });
});

describe('storage', () => {
  afterEach(() => {
    localStorage.clear();
    vi.restoreAllMocks();
  });

  const stored = () => JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null') as unknown;

  it('remembers the years with the genres, and leaves them out when unset', () => {
    writeStored({ model: null, genres: ['Horror'], years: NINETIES });
    expect(stored()).toEqual({ genres: ['Horror'], year_from: 1990, year_to: 1999 });
    writeStored({ model: 'knn', genres: [], years: { from: null, to: 1999 } });
    expect(stored()).toEqual({ model: 'knn', genres: [], year_to: 1999 });
    writeStored({ model: null, genres: [], years: NO_YEARS });
    expect(localStorage.getItem(STORAGE_KEY)).toBe('{"genres":[]}');
  });

  it('reads back what it wrote', () => {
    const choice = { model: 'mf', genres: ['War' as const], years: { from: 1940, to: 1949 } };
    writeStored(choice);
    expect(readStored()).toEqual(choice);
  });

  it('reads an older value without year keys', () => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ model: 'content', genres: ['Comedy'] }));
    expect(readStored()).toEqual({ model: 'content', genres: ['Comedy'], years: NO_YEARS });
  });

  it.each([
    ['strings', { year_from: '1990', year_to: '1999' }, NINETIES],
    ['out of range', { year_from: 1700, year_to: 1999 }, NO_YEARS],
    ['fractions', { year_from: 1990.5 }, NO_YEARS],
    ['an inverted range', { year_from: 2000, year_to: 1990 }, NO_YEARS],
    ['the wrong types', { year_from: true, year_to: [1999] }, NO_YEARS],
  ])('cleans stored years given as %s', (_, value, years) => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ genres: [], ...value }));
    expect(readStored().years).toEqual(years);
  });

  it('gives an empty choice when storage is unavailable', () => {
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new DOMException('denied', 'SecurityError');
    });
    expect(readStored()).toEqual({ model: null, genres: [], years: NO_YEARS });
  });
});
