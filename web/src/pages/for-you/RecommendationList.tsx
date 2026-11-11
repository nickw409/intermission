// For you's list: fetches /api/recommend for one model and genre filter, and shows the cards with
// their explanation, rate control and "Not interested". A polite live region announces the count
// whenever a new list arrives (it stays mounted while the list reloads, so the change is heard).
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
import { genrePhrase, parseGenres } from './choice';

type RecommendResponse = Schemas['RecommendResponse'];
type RecItem = Schemas['RecItem'];

const TOP_K = 20;

function fetchRecommendations(model: ModelName, genres: string, signal: AbortSignal): Promise<RecommendResponse> {
  return api.get('/api/recommend', { query: { model, k: TOP_K, ...(genres ? { genres } : {}) }, signal });
}

/** The list for one model and filter. An unknown_model answer (the model list was unavailable and
 * knn is not loaded) falls back to pop. */
function useRecommendations(model: ModelName, genresKey: string) {
  const load = useCallback(
    async (signal: AbortSignal) => {
      try {
        return await fetchRecommendations(model, genresKey, signal);
      } catch (error) {
        if (!(error instanceof ApiError && error.code === 'unknown_model' && model !== 'pop')) throw error;
        return fetchRecommendations('pop', genresKey, signal);
      }
    },
    [model, genresKey],
  );
  return useAsync(load);
}

const picks = (n: number) => (n === 1 ? 'pick' : 'picks');

export function RecommendationList({
  model,
  genresKey,
  onClearGenres,
}: {
  model: ModelName;
  /** Comma-joined genres in contract order, '' for no filter. */
  genresKey: string;
  onClearGenres: () => void;
}) {
  const { state, reload } = useRecommendations(model, genresKey);
  const ratings = useOptimisticRatings();
  const [hidden, setHidden] = useState<ReadonlySet<number>>(new Set());
  useRefetchAfterRating(() => reload({ quiet: true }));

  const phrase = genrePhrase(parseGenres(genresKey));
  const visible = state.status === 'ready' ? state.data.items.filter((item) => !hidden.has(item.movieId)) : [];
  const announcement =
    state.status === 'ready'
      ? `${visible.length} ${phrase ? `${phrase} ` : ''}${picks(visible.length)} from ${modelLabel(state.data.model)}`
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
        phrase={phrase}
        ratings={ratings}
        onClearGenres={onClearGenres}
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
  phrase,
  ratings,
  onClearGenres,
  onHide,
}: {
  data: RecommendResponse;
  items: RecItem[];
  busy: boolean;
  /** The chosen genres as words ("action or horror"), '' for no filter. */
  phrase: string;
  ratings: OptimisticRatings;
  onClearGenres: () => void;
  onHide: (movieId: number) => void;
}) {
  return (
    <>
      <p className="for-you-summary">
        {`Picks from ${modelLabel(data.model)}`}
        {phrase && <span className="muted"> · {phrase}</span>}
      </p>
      {data.cold_start && (
        <div className="notice">
          <p>These are popular picks while we get to know you. Rate at least 5 movies to get recommendations made for you.</p>
          <Link to="/onboarding" className="button button--primary">
            Rate more movies
          </Link>
        </div>
      )}
      {phrase && items.length > 0 && items.length < TOP_K && (
        <p className="notice">
          Only {items.length} {phrase} {picks(items.length)} right now — rate more movies or widen the categories.
        </p>
      )}
      {items.length === 0 ? (
        <NothingToShow phrase={phrase} onClearGenres={onClearGenres} />
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

function NothingToShow({ phrase, onClearGenres }: { phrase: string; onClearGenres: () => void }) {
  if (phrase) {
    return (
      <Empty>
        <p>No {phrase} picks right now.</p>
        <p>
          <button type="button" className="button" onClick={onClearGenres}>
            Clear categories
          </button>
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
