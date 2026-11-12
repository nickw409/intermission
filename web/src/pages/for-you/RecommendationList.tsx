// For you's list: fetches /api/recommend for one model and filter (genres and release years), and
// shows the cards with their explanation, rate control and "Not interested". A polite live region
// announces the count whenever a new list arrives (it stays mounted while the list reloads, so the
// change is heard).
import { useCallback, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router';
import { api, ApiError, type Schemas } from '../../api/client';
import { MovieCard } from '../../components/MovieCard';
import { RateMovie } from '../../components/RateMovie';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { friendlyMessage } from '../../lib/errors';
import { feedbackHook } from '../../lib/feedback';
import { displayTitle, joinTitles } from '../../lib/format';
import { modelLabel, type ModelName } from '../../lib/models';
import { useOptimisticRatings, useRefetchAfterRating, type OptimisticRatings } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import { filterQuery, genrePhrase, hasYears, parseFilter, releasedPhrase, yearPhrase, type Filter } from './choice';

type RecommendResponse = Schemas['RecommendResponse'];
type RecItem = Schemas['RecItem'];

const TOP_K = 20;

function fetchRecommendations(model: ModelName, filter: Filter, signal: AbortSignal): Promise<RecommendResponse> {
  return api.get('/api/recommend', { query: { model, k: TOP_K, ...filterQuery(filter) }, signal });
}

/** The list for one model and filter. An unknown_model answer (the model list was unavailable and
 * knn is not loaded) falls back to pop. */
function useRecommendations(model: ModelName, filterKey: string) {
  const load = useCallback(
    async (signal: AbortSignal) => {
      const filter = parseFilter(new URLSearchParams(filterKey));
      try {
        return await fetchRecommendations(model, filter, signal);
      } catch (error) {
        if (!(error instanceof ApiError && error.code === 'unknown_model' && model !== 'pop')) throw error;
        return fetchRecommendations('pop', filter, signal);
      }
    },
    [model, filterKey],
  );
  return useAsync(load);
}

const picks = (n: number) => (n === 1 ? 'pick' : 'picks');

/** Joins the non-empty parts with `separator`. */
const joinParts = (parts: (string | number)[], separator = ' ') => parts.filter((p) => p !== '').join(separator);

/** The filter in words, for the summary line and the sentences around the list. */
interface FilterWords {
  /** The genres ("action or horror"), '' for none. */
  genres: string;
  /** Short years for the summary ("1990–1999", "from 1990"), '' for none. */
  years: string;
  /** Years for sentences ("released 1990–1999"), '' for none. */
  released: string;
  /** What the user could widen: "categories", "years", "categories or years", or '' for no filter. */
  widen: string;
}

function filterWords(filter: Filter): FilterWords {
  const genres = genrePhrase(filter.genres);
  return {
    genres,
    years: yearPhrase(filter.years),
    released: releasedPhrase(filter.years),
    widen: joinParts([genres ? 'categories' : '', hasYears(filter.years) ? 'years' : ''], ' or '),
  };
}

export function RecommendationList({
  model,
  filterKey,
  onClearGenres,
  onClearYears,
}: {
  model: ModelName;
  /** The filter as a query string ("genres=Horror&year_from=1990"), '' for none (see filterParams). */
  filterKey: string;
  onClearGenres: () => void;
  onClearYears: () => void;
}) {
  const { state, reload } = useRecommendations(model, filterKey);
  const ratings = useOptimisticRatings();
  const [hidden, setHidden] = useState<ReadonlySet<number>>(new Set());
  // Movies whose "Not interested" could not be saved: shown again, with an error.
  const [failed, setFailed] = useState<ReadonlySet<number>>(new Set());
  // The movie just hidden, offered for undo, and a problem with an undo.
  const [justHidden, setJustHidden] = useState<RecItem | null>(null);
  const [undoProblem, setUndoProblem] = useState<string | null>(null);
  // The card to focus once an undo has brought it back (read after the next render).
  const focusAfterUndo = useRef<number | null>(null);
  const undoRef = useRef<HTMLButtonElement>(null);
  // Errors and the undo offer belong to the list they happened on, so a new model or filter
  // starts without them (and a late failure from the old list is not shown on the new one).
  const listKey = `${model}?${filterKey}`;
  const listKeyRef = useRef(listKey);
  const [shownFor, setShownFor] = useState(listKey);
  if (shownFor !== listKey) {
    setShownFor(listKey);
    setFailed(new Set());
    setJustHidden(null);
    setUndoProblem(null);
  }
  useEffect(() => {
    listKeyRef.current = listKey;
  }, [listKey]);

  // Each movie's requests run one after another (a hide, its undo, a second hide...), so they
  // reach the server in the order the user clicked. Only the newest click on a movie may change
  // what its card shows when its request fails.
  const queues = useRef(new Map<number, Promise<unknown>>());
  const clicks = useRef(new Map<number, number>());
  const enqueue = (movieId: number, request: () => Promise<void>) => {
    const run = (queues.current.get(movieId) ?? Promise.resolve()).catch(() => undefined).then(request);
    queues.current.set(movieId, run);
    return run;
  };
  const click = (movieId: number) => {
    const n = (clicks.current.get(movieId) ?? 0) + 1;
    clicks.current.set(movieId, n);
    const key = listKeyRef.current;
    return { latest: () => clicks.current.get(movieId) === n, sameList: () => listKeyRef.current === key };
  };
  const setIn = (set: ReadonlySet<number>, movieId: number, present: boolean) => {
    const next = new Set(set);
    if (present) next.add(movieId);
    else next.delete(movieId);
    return next;
  };

  const notInterested = (item: RecItem, listModel: ModelName) => {
    const me = click(item.movieId);
    setHidden((current) => setIn(current, item.movieId, true));
    setFailed((current) => setIn(current, item.movieId, false));
    setJustHidden(item);
    setUndoProblem(null);
    enqueue(item.movieId, () => feedbackHook.notInterested({ movieId: item.movieId, model: listModel })).catch(() => {
      if (!me.latest()) return; // undone (or clicked again) since: that click decides
      // Not saved: the card comes back with an error, and there is nothing to undo.
      setHidden((current) => setIn(current, item.movieId, false));
      if (me.sameList()) setFailed((current) => setIn(current, item.movieId, true));
      setJustHidden((current) => (current?.movieId === item.movieId ? null : current));
    });
  };

  const undo = (item: RecItem) => {
    const me = click(item.movieId);
    setJustHidden(null);
    setUndoProblem(null);
    setHidden((current) => setIn(current, item.movieId, false));
    setFailed((current) => setIn(current, item.movieId, false));
    focusAfterUndo.current = item.movieId;
    enqueue(item.movieId, () => feedbackHook.undoNotInterested(item.movieId)).then(
      // The list may have been refetched without the movie meanwhile (after a rating): fetch it
      // again so the movie can come back.
      () => reload({ quiet: true }),
      (error: unknown) => {
        if (!me.latest()) return;
        setHidden((current) => setIn(current, item.movieId, true));
        if (me.sameList()) {
          setUndoProblem(
            `Couldn't undo "Not interested" for ${displayTitle(item.title)}. ${friendlyMessage(error)} You can also undo it on your Ratings page.`,
          );
        }
      },
    );
  };

  // After hiding, focus goes to Undo; after an undo, to the card that came back (or the heading).
  useEffect(() => {
    if (justHidden) undoRef.current?.focus();
  }, [justHidden]);
  useEffect(() => {
    const movieId = focusAfterUndo.current;
    if (movieId === null || hidden.has(movieId)) return;
    focusAfterUndo.current = null;
    const link = document.querySelector<HTMLElement>(`.card-grid [data-movie-id="${movieId}"] a`);
    if (link) link.focus();
    else document.querySelector<HTMLElement>('main h1')?.focus();
  }, [hidden]);
  useRefetchAfterRating(() => reload({ quiet: true }));

  const words = filterWords(parseFilter(new URLSearchParams(filterKey)));
  const visible = state.status === 'ready' ? state.data.items.filter((item) => !hidden.has(item.movieId)) : [];
  // "20 horror picks from Implicit ALS, released 1990–1999"
  const announcement =
    state.status === 'ready'
      ? joinParts(
          [
            joinParts([visible.length, words.genres, picks(visible.length), 'from', modelLabel(state.data.model)]),
            words.released,
          ],
          ', ',
        )
      : '';

  let body;
  if (state.status === 'loading') body = <Loading>Loading your recommendations…</Loading>;
  else if (state.status === 'error') body = <ErrorMessage message={state.message} onRetry={() => reload()} />;
  else {
    body = (
      <Results
        data={state.data}
        items={visible}
        busy={state.refreshing}
        words={words}
        ratings={ratings}
        onClearGenres={onClearGenres}
        onClearYears={onClearYears}
        failed={failed}
        onNotInterested={notInterested}
      />
    );
  }
  return (
    <>
      <p className="visually-hidden" aria-live="polite">
        {announcement}
      </p>
      <div role="status" className="undo-region">
        {justHidden && (
          <div className="notice notice--inline">
            <p>You won't see {displayTitle(justHidden.title)} in your recommendations.</p>
            <button
              ref={undoRef}
              type="button"
              className="button"
              aria-label={`Undo not interested in ${displayTitle(justHidden.title)}`}
              onClick={() => undo(justHidden)}
            >
              Undo
            </button>
          </div>
        )}
      </div>
      {undoProblem && <ErrorMessage message={undoProblem} />}
      {body}
    </>
  );
}

function Results({
  data,
  items,
  busy,
  words,
  ratings,
  onClearGenres,
  onClearYears,
  failed,
  onNotInterested,
}: {
  data: RecommendResponse;
  items: RecItem[];
  busy: boolean;
  words: FilterWords;
  ratings: OptimisticRatings;
  onClearGenres: () => void;
  onClearYears: () => void;
  failed: ReadonlySet<number>;
  onNotInterested: (item: RecItem, model: ModelName) => void;
}) {
  return (
    <>
      <p className="for-you-summary">
        {`Picks from ${modelLabel(data.model)}`}
        {words.genres && <span className="muted"> · {words.genres}</span>}
        {words.years && <span className="muted"> · {words.years}</span>}
      </p>
      {data.cold_start && (
        <div className="notice">
          <p>These are popular picks while we get to know you. Rate at least 5 movies to get recommendations made for you.</p>
          <Link to="/onboarding" className="button button--primary">
            Rate more movies
          </Link>
        </div>
      )}
      {words.widen && items.length > 0 && items.length < TOP_K && (
        <p className="notice">
          Only {joinParts([items.length, words.genres, picks(items.length), words.released])} right now — rate more
          movies or widen the {words.widen}.
        </p>
      )}
      {items.length === 0 ? (
        <NothingToShow words={words} onClearGenres={onClearGenres} onClearYears={onClearYears} />
      ) : (
        <ul className="card-grid" aria-busy={busy || undefined} aria-label="Recommendations">
          {items.map((item) => (
            <li key={item.movieId} data-movie-id={item.movieId}>
              <RecommendationCard
                item={item}
                model={data.model}
                ratings={ratings}
                hideFailed={failed.has(item.movieId)}
                onNotInterested={onNotInterested}
              />
            </li>
          ))}
        </ul>
      )}
    </>
  );
}

function NothingToShow({
  words,
  onClearGenres,
  onClearYears,
}: {
  words: FilterWords;
  onClearGenres: () => void;
  onClearYears: () => void;
}) {
  if (words.widen) {
    return (
      <Empty>
        <p>No {joinParts([words.genres, 'picks', words.released])} right now.</p>
        <p className="for-you-empty-actions">
          {words.genres && (
            <button type="button" className="button" onClick={onClearGenres}>
              Clear categories
            </button>
          )}
          {words.released && (
            <button type="button" className="button" onClick={onClearYears}>
              Clear years
            </button>
          )}
        </p>
      </Empty>
    );
  }
  return (
    <Empty>
      <p>No recommendations right now.</p>
      <p>
        <Link to="/onboarding">Rate a few more movies</Link> and check back.
      </p>
    </Empty>
  );
}

function RecommendationCard({
  item,
  model,
  ratings,
  hideFailed,
  onNotInterested,
}: {
  item: RecItem;
  model: ModelName;
  ratings: OptimisticRatings;
  /** The last "Not interested" for this movie could not be saved. */
  hideFailed: boolean;
  onNotInterested: (item: RecItem, model: ModelName) => void;
}) {
  // Hidden at once (focus moves to the Undo offer above the list); if the server could not save
  // it, the card comes back with an error.
  const notInterested = () => onNotInterested(item, model);
  return (
    <MovieCard movieId={item.movieId} title={item.title} year={item.year}>
      {item.because.length > 0 && (
        <p className="because">Because you rated {joinTitles(item.because.map((b) => displayTitle(b.title)))}</p>
      )}
      <RateMovie
        title={item.title}
        value={ratings.ratingOf(item.movieId, null)}
        onRate={(next) => void ratings.rate(item.movieId, next, ratings.ratingOf(item.movieId, null))}
        error={ratings.errorOf(item.movieId)}
      />
      <button
        type="button"
        className="button button--quiet button--small"
        aria-label={`Not interested in ${displayTitle(item.title)}`}
        onClick={notInterested}
      >
        Not interested
      </button>
      {hideFailed && (
        <p role="alert" className="field__error">
          Couldn't save &quot;Not interested&quot;. Try again.
        </p>
      )}
    </MovieCard>
  );
}
