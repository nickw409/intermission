// My ratings: every rating the user has made (GET /api/ratings), sortable by date, title or rating.
// Ratings are edited in place with the star control and deleted with an Undo. Below them, the
// movies the user marked "Not interested" on For you, each with an Undo. All of this goes through
// recs-api alone, so the page keeps working while the model service is down.
import { useCallback, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router';
import { api, type Schemas } from '../../api/client';
import { RateMovie } from '../../components/RateMovie';
import { RequireSession } from '../../components/RequireSession';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { friendlyMessage } from '../../lib/errors';
import { feedbackHook } from '../../lib/feedback';
import { displayTitle, formatDate } from '../../lib/format';
import { saveRating, useOptimisticRatings } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import '../../components/components.css';

type Rating = Schemas['Rating'];
type Hidden = Schemas['NotInterested'];
type RatingList = Schemas['RatingList'];
type SortKey = 'date' | 'title' | 'rating';
type Direction = 'ascending' | 'descending';

const COLUMNS: { key: SortKey; label: string; first: Direction }[] = [
  { key: 'title', label: 'Title', first: 'ascending' },
  { key: 'rating', label: 'Rating', first: 'descending' },
  { key: 'date', label: 'Rated on', first: 'descending' },
];

export default function RatingsPage() {
  return (
    <div className="page">
      <h1>My ratings</h1>
      <RequireSession action="see your ratings">
        {() => (
          <>
            <Ratings />
            <NotInterestedSection />
          </>
        )}
      </RequireSession>
    </div>
  );
}

function compareBy(key: SortKey, valueOf: (r: Rating) => number): (a: Rating, b: Rating) => number {
  const byDate = (a: Rating, b: Rating) => a.updated_at - b.updated_at;
  switch (key) {
    case 'date':
      return byDate;
    // MovieLens titles put the article last ("Matrix, The"), so this sorts by the first real word.
    case 'title':
      return (a, b) => a.title.localeCompare(b.title);
    case 'rating':
      return (a, b) => valueOf(a) - valueOf(b) || byDate(a, b);
  }
}

const without = (movieId: number) => (data: RatingList) => ({ items: data.items.filter((r) => r.movieId !== movieId) });
const withRow = (row: Rating) => (data: RatingList) => ({ items: [...without(row.movieId)(data).items, row] });

function Ratings() {
  const load = useCallback((signal: AbortSignal) => api.get('/api/ratings', { signal }), []);
  const { state, reload, update } = useAsync(load);
  const ratings = useOptimisticRatings();
  const [sort, setSort] = useState<{ key: SortKey; direction: Direction }>({ key: 'date', direction: 'descending' });
  const [deleted, setDeleted] = useState<Rating | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [focusMovie, setFocusMovie] = useState<number | null>(null);
  const undoRef = useRef<HTMLButtonElement>(null);
  const tableRef = useRef<HTMLTableElement>(null);

  useEffect(() => {
    if (deleted) undoRef.current?.focus();
  }, [deleted]);

  useEffect(() => {
    if (focusMovie === null) return;
    tableRef.current?.querySelector<HTMLElement>(`[data-movie-id="${focusMovie}"] a`)?.focus();
  }, [focusMovie]);

  if (state.status === 'loading') return <Loading>Loading your ratings…</Loading>;
  if (state.status === 'error') return <ErrorMessage message={state.message} onRetry={() => reload()} />;

  const valueOf = (r: Rating) => ratings.ratingOf(r.movieId, r.rating) ?? r.rating;

  const remove = async (row: Rating) => {
    const shown = { ...row, rating: valueOf(row) };
    update(without(row.movieId));
    setDeleted(null);
    setProblem(null);
    try {
      await saveRating(row.movieId, null);
      setDeleted(shown);
    } catch (error) {
      update(withRow(shown));
      setProblem(`Couldn't delete your rating for ${displayTitle(row.title)}. ${friendlyMessage(error)}`);
    }
  };

  const restore = async (row: Rating) => {
    setDeleted(null);
    setProblem(null);
    update(withRow(row));
    setFocusMovie(row.movieId);
    try {
      await saveRating(row.movieId, row.rating);
    } catch (error) {
      update(without(row.movieId));
      setProblem(`Couldn't restore your rating for ${displayTitle(row.title)}. ${friendlyMessage(error)}`);
    }
  };

  const onSort = (key: SortKey, first: Direction) =>
    setSort((current) =>
      current.key === key
        ? { key, direction: current.direction === 'ascending' ? 'descending' : 'ascending' }
        : { key, direction: first },
    );

  const compare = compareBy(sort.key, valueOf);
  const rows = [...state.data.items].sort((a, b) => (sort.direction === 'ascending' ? compare(a, b) : compare(b, a)));

  return (
    <>
      <div role="status" className="undo-region">
        {deleted && (
          <div className="notice notice--inline">
            <p>Deleted your rating for {displayTitle(deleted.title)}.</p>
            <button ref={undoRef} type="button" className="button" onClick={() => void restore(deleted)}>
              Undo
            </button>
          </div>
        )}
      </div>
      {problem && <ErrorMessage message={problem} />}
      {rows.length === 0 ? (
        <Empty>
          <p>You haven't rated any movies yet.</p>
          <Link to="/onboarding" className="button button--primary">
            Rate some movies
          </Link>
        </Empty>
      ) : (
        <div className="table-scroll">
          <table ref={tableRef} className="ratings-table">
            <caption className="visually-hidden">
              Your ratings, sorted by {COLUMNS.find((c) => c.key === sort.key)?.label.toLowerCase()}, {sort.direction}
            </caption>
            <thead>
              <tr>
                {COLUMNS.map((column) => (
                  <th key={column.key} scope="col" aria-sort={sort.key === column.key ? sort.direction : undefined}>
                    <button type="button" className="sort-button" onClick={() => onSort(column.key, column.first)}>
                      {column.label}
                      <span aria-hidden="true" className="sort-button__arrow">
                        {sort.key === column.key ? (sort.direction === 'ascending' ? '▲' : '▼') : ''}
                      </span>
                    </button>
                  </th>
                ))}
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.movieId}>
                  <td data-movie-id={row.movieId}>
                    <Link to={`/movies/${row.movieId}`}>{displayTitle(row.title)}</Link>
                    {row.year !== null && <span className="muted"> ({row.year})</span>}
                  </td>
                  <td>
                    <RateMovie
                      title={row.title}
                      value={valueOf(row)}
                      onRate={(next) =>
                        next === null ? void remove(row) : void ratings.rate(row.movieId, next, valueOf(row))
                      }
                      error={ratings.errorOf(row.movieId)}
                    />
                  </td>
                  <td>
                    <time dateTime={new Date(row.updated_at * 1000).toISOString()}>{formatDate(row.updated_at)}</time>
                  </td>
                  <td>
                    <button
                      type="button"
                      className="button button--quiet button--small"
                      aria-label={`Delete rating for ${displayTitle(row.title)}`}
                      onClick={() => void remove(row)}
                    >
                      Delete
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </>
  );
}

/** The movies marked "Not interested", newest first, each with an Undo. Hidden while empty. */
function NotInterestedSection() {
  const load = useCallback((signal: AbortSignal) => api.get('/api/feedback/not-interested', { signal }), []);
  const { state, reload, update } = useAsync(load);
  const [problem, setProblem] = useState<string | null>(null);
  const [undone, setUndone] = useState<string | null>(null);
  // Where focus goes once an undone movie's row is gone: the next row's Undo, else the heading.
  const focusNext = useRef<number | null>(null);
  const sectionRef = useRef<HTMLElement>(null);
  const items = state.status === 'ready' ? state.data.items : null;
  useEffect(() => {
    if (focusNext.current === null || items === null) return;
    const target = focusNext.current;
    focusNext.current = null;
    const button = sectionRef.current?.querySelector<HTMLElement>(`[data-movie-id="${target}"] button`);
    if (button) button.focus();
    else sectionRef.current?.querySelector<HTMLElement>('h2')?.focus();
  }, [items]);

  if (state.status === 'loading') return null;
  if (state.status === 'error') {
    return (
      <section aria-labelledby="not-interested-heading" className="not-interested">
        <h2 id="not-interested-heading">Not interested</h2>
        <ErrorMessage message={state.message} onRetry={() => reload()} />
      </section>
    );
  }
  if (state.data.items.length === 0 && !undone && !problem) return null;

  const undo = async (movie: Hidden) => {
    const without = (data: { items: Hidden[] }) => ({ items: data.items.filter((m) => m.movieId !== movie.movieId) });
    const list = state.data.items;
    const at = list.findIndex((m) => m.movieId === movie.movieId);
    focusNext.current = (list[at + 1] ?? list[at - 1])?.movieId ?? -1;
    update(without);
    setProblem(null);
    try {
      await feedbackHook.undoNotInterested(movie.movieId);
      setUndone(`${displayTitle(movie.title)} can be recommended to you again.`);
    } catch (error) {
      update((data) => ({ items: [...without(data).items, movie].sort((a, b) => b.hidden_at - a.hidden_at) }));
      setProblem(`Couldn't undo "Not interested" for ${displayTitle(movie.title)}. ${friendlyMessage(error)}`);
    }
  };

  return (
    <section ref={sectionRef} aria-labelledby="not-interested-heading" className="not-interested">
      <h2 id="not-interested-heading" tabIndex={-1}>
        Not interested
      </h2>
      <p className="muted">Movies you hid on For you. They are left out of your recommendations.</p>
      <p role="status">{undone}</p>
      {problem && <ErrorMessage message={problem} />}
      {state.data.items.length > 0 && (
        <ul className="not-interested__list">
          {state.data.items.map((movie) => (
            <li key={movie.movieId} data-movie-id={movie.movieId}>
              <Link to={`/movies/${movie.movieId}`}>{displayTitle(movie.title)}</Link>
              {movie.year !== null && <span className="muted"> ({movie.year})</span>}{' '}
              <button
                type="button"
                className="button button--quiet button--small"
                aria-label={`Undo not interested in ${displayTitle(movie.title)}`}
                onClick={() => void undo(movie)}
              >
                Undo
              </button>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
