// For you: the signed-in user's top 20 from the default model (GET /api/models default_model,
// falling back to knn, then pop). Each poster card explains itself ("Because you rated …"), can be
// rated in place, and can be dismissed with "Not interested". The list refetches 500 ms after a
// rating change is saved, so newly rated movies drop out and the rest re-rank.
import { useCallback, useRef, useState, type MouseEvent } from 'react';
import { Link } from 'react-router';
import { api, ApiError, type Schemas } from '../../api/client';
import { MovieCard } from '../../components/MovieCard';
import { RateMovie } from '../../components/RateMovie';
import { RequireSession } from '../../components/RequireSession';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { feedbackHook } from '../../lib/feedback';
import { displayTitle, joinTitles } from '../../lib/format';
import { pickDefaultModel, type ModelName } from '../../lib/models';
import { useOptimisticRatings, useRefetchAfterRating } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import '../../components/components.css';

type RecommendResponse = Schemas['RecommendResponse'];
type RecItem = Schemas['RecItem'];

const TOP_K = 20;

async function fetchRecommendations(model: ModelName, signal: AbortSignal): Promise<RecommendResponse> {
  return api.get('/api/recommend', { query: { model, k: TOP_K }, signal });
}

export default function ForYouPage() {
  return (
    <div className="page">
      <h1 tabIndex={-1}>For you</h1>
      <RequireSession action="see your recommendations">{() => <Recommendations />}</RequireSession>
    </div>
  );
}

function Recommendations() {
  // The model is chosen once; refetches after a rating reuse it.
  const chosen = useRef<ModelName | null>(null);
  const load = useCallback(async (signal: AbortSignal) => {
    let model = chosen.current;
    if (model === null) {
      const models = await api.get('/api/models', { signal }).catch((error: unknown) => {
        if (signal.aborted) throw error;
        return null;
      });
      model = pickDefaultModel(models);
    }
    let response: RecommendResponse;
    try {
      response = await fetchRecommendations(model, signal);
    } catch (error) {
      if (!(error instanceof ApiError && error.code === 'unknown_model' && model !== 'pop')) throw error;
      model = 'pop';
      response = await fetchRecommendations(model, signal);
    }
    chosen.current = model;
    return response;
  }, []);

  const { state, reload } = useAsync(load);
  const ratings = useOptimisticRatings();
  const [hidden, setHidden] = useState<ReadonlySet<number>>(new Set());
  useRefetchAfterRating(() => reload({ quiet: true }));

  if (state.status === 'loading') return <Loading>Loading your recommendations…</Loading>;
  if (state.status === 'error') return <ErrorMessage message={state.message} onRetry={() => reload()} />;

  const { data } = state;
  const items = data.items.filter((item) => !hidden.has(item.movieId));

  const notInterested = (item: RecItem, event: MouseEvent<HTMLButtonElement>) => {
    // Move focus to a neighbouring card before this one disappears.
    const li = event.currentTarget.closest('li');
    const neighbour = li?.nextElementSibling ?? li?.previousElementSibling;
    const target = neighbour?.querySelector<HTMLElement>('a');
    if (target) target.focus();
    else document.querySelector<HTMLElement>('main h1')?.focus();
    feedbackHook.notInterested({ movieId: item.movieId, model: data.model });
    setHidden((current) => new Set(current).add(item.movieId));
  };

  return (
    <>
      {data.cold_start && (
        <div className="notice">
          <p>These are popular picks while we get to know you. Rate at least 5 movies to get recommendations made for you.</p>
          <Link to="/onboarding" className="button button--primary">
            Rate more movies
          </Link>
        </div>
      )}
      {items.length === 0 ? (
        <Empty>
          <p>No recommendations right now.</p>
          <p>
            <Link to="/onboarding">Rate a few more movies</Link> and check back.
          </p>
        </Empty>
      ) : (
        <ul className="card-grid" aria-busy={state.refreshing || undefined} aria-label="Recommendations">
          {items.map((item) => (
            <li key={item.movieId}>
              <MovieCard movieId={item.movieId} title={item.title} year={item.year}>
                {item.because.length > 0 && (
                  <p className="because">
                    Because you rated {joinTitles(item.because.map((b) => displayTitle(b.title)))}
                  </p>
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
                  onClick={(event) => notInterested(item, event)}
                >
                  Not interested
                </button>
              </MovieCard>
            </li>
          ))}
        </ul>
      )}
    </>
  );
}
