// Small text helpers for movie titles and dates.

// MovieLens files titles with a trailing English article ("Matrix, The") so they sort by the
// first real word. People read them the other way round.
const TRAILING_ARTICLE = /^(.*), (The|A|An)$/;

/** "Matrix, The" → "The Matrix". Anything else is returned unchanged. */
export function displayTitle(title: string): string {
  const match = TRAILING_ARTICLE.exec(title);
  return match ? `${match[2]} ${match[1]}` : title;
}

/** "The Matrix (1999)", or just the title when the year is unknown. */
export function titleWithYear(title: string, year: number | null): string {
  const shown = displayTitle(title);
  return year === null ? shown : `${shown} (${year})`;
}

/** ["A", "B", "C"] → "A, B and C". */
export function joinTitles(titles: string[]): string {
  if (titles.length <= 1) return titles.join('');
  return `${titles.slice(0, -1).join(', ')} and ${titles.at(-1)}`;
}

const DATE = new Intl.DateTimeFormat(undefined, { year: 'numeric', month: 'short', day: 'numeric' });

/** Unix seconds → a short local date such as "Nov 9, 2026". */
export function formatDate(unixSeconds: number): string {
  return DATE.format(new Date(unixSeconds * 1000));
}
