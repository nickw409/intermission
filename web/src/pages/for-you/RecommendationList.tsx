// For you's list: fetches /api/recommend for one model and filter (genres and release years), and
// shows the cards with their explanation, rate control and "Not interested". A polite live region
// announces the count whenever a new list arrives (it stays mounted while the list reloads, so the
// change is heard).
import { useCallback, useState, type MouseEvent } from 'react';
import { Link } from 'react-router';
import { api, ApiError, type Schemas } from '../../api/client';
import { MovieCard } from '../../components/MovieCard';
import { RateMovie } from '../../components/RateMovie';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
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
        onHide={(movieId) => setHidden((current) => new Set(current).add(movieId))}
      />
    );
  }
  return (
    <>
      <p className="visually-hidden" aria-live="polite">
        {announcement}
      </p>
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
  onHide,
}: {
  data: RecommendResponse;
  items: RecItem[];
  busy: boolean;
  words: FilterWords;
  ratings: OptimisticRatings;
  onClearGenres: () => void;
  onClearYears: () => void;
  onHide: (movieId: number) => void;
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
            <li key={item.movieId}>
              <RecommendationCard item={item} model={data.model} ratings={ratings} onHide={onHide} />
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
  onHide,
}: {
  item: RecItem;
  model: ModelName;
  ratings: OptimisticRatings;
  onHide: (movieId: number) => void;
}) {
  const notInterested = (event: MouseEvent<HTMLButtonElement>) => {
    // Move focus to a neighbouring card before this one disappears.
    const li = event.currentTarget.closest('li');
    const neighbour = li?.nextElementSibling ?? li?.previousElementSibling;
    const target = neighbour?.querySelector<HTMLElement>('a');
    if (target) target.focus();
    else document.querySelector<HTMLElement>('main h1')?.focus();
    feedbackHook.notInterested({ movieId: item.movieId, model });
    onHide(item.movieId);
  };
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
    </MovieCard>
  );
}
