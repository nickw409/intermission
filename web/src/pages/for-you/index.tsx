// For you: the signed-in user's top 20. The model defaults to the server's (GET /api/models
// default_model, falling back to knn, then pop), and the user can pick any loaded model and narrow
// the list with genre chips; both choices live in the URL and are remembered per browser (see
// choice.ts). Each poster card explains itself ("Because you rated …"), can be rated in place, and
// can be dismissed with "Not interested". The list refetches 500 ms after a rating change is saved,
// so newly rated movies drop out and the rest re-rank.
import { useCallback, useEffect, useId, useRef, useState, type MouseEvent, type RefObject } from 'react';
import { Link, useSearchParams } from 'react-router';
import { api, ApiError, type Schemas } from '../../api/client';
import { MovieCard } from '../../components/MovieCard';
import { RateMovie } from '../../components/RateMovie';
import { RequireSession } from '../../components/RequireSession';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { feedbackHook } from '../../lib/feedback';
import { displayTitle, joinTitles } from '../../lib/format';
import { modelLabel, pickDefaultModel, type ModelName, type ModelsResponse } from '../../lib/models';
import { useOptimisticRatings, useRefetchAfterRating } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import {
  CHIP_ORDER,
  cleanGenres,
  fromSearchParams,
  genrePhrase,
  genresParam,
  readStored,
  resolveChoice,
  toSearchParams,
  writeStored,
  type Choice,
  type Genre,
} from './choice';
import '../../components/components.css';
import './for-you.css';

type RecommendResponse = Schemas['RecommendResponse'];
type RecItem = Schemas['RecItem'];

const TOP_K = 20;

async function fetchRecommendations(
  model: ModelName,
  genres: string | undefined,
  signal: AbortSignal,
): Promise<RecommendResponse> {
  return api.get('/api/recommend', { query: { model, k: TOP_K, ...(genres ? { genres } : {}) }, signal });
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
  // Without a model list the page still works: no selector, and the default fallback chain.
  const loadModels = useCallback(
    (signal: AbortSignal) =>
      api.get('/api/models', { signal }).catch((error: unknown) => {
        if (signal.aborted) throw error;
        return null;
      }),
    [],
  );
  const { state } = useAsync(loadModels);
  if (state.status !== 'ready') return <Loading>Loading your recommendations…</Loading>;
  return <ForYou models={state.data} />;
}

function ForYou({ models }: { models: ModelsResponse | null }) {
  const [params, setParams] = useSearchParams();
  // The remembered choice, kept in step with what the user picks on this page.
  const [stored, setStored] = useState(readStored);
  const choice = resolveChoice(fromSearchParams(params), stored, models, pickDefaultModel(models));
  const canonical = toSearchParams(choice).toString();
  const genresKey = genresParam(choice.genres);

  // Show the view actually displayed in the URL (drops invalid values, fills in a remembered
  // choice), so it can be copied and shared as is. Without a model list the model is a guess.
  useEffect(() => {
    if (models !== null && params.toString() !== canonical) setParams(canonical, { replace: true });
  }, [models, params, canonical, setParams]);

  const choose = (next: Choice) => {
    setStored({ model: next.model, genres: next.genres });
    writeStored(next);
    setParams(toSearchParams(next), { replace: true });
  };

  // Both Clear buttons disappear or become disabled once used, so focus moves to the first chip
  // rather than being lost.
  const firstChip = useRef<HTMLButtonElement>(null);
  const clearGenres = () => {
    firstChip.current?.focus();
    choose({ ...choice, genres: [] });
  };

  return (
    <>
      <Controls models={models} choice={choice} onChange={choose} onClear={clearGenres} firstChip={firstChip} />
      <RecommendationList model={choice.model} genresKey={genresKey} genres={choice.genres} onClearGenres={clearGenres} />
    </>
  );
}

function Controls({
  models,
  choice,
  onChange,
  onClear,
  firstChip,
}: {
  models: ModelsResponse | null;
  choice: Choice;
  onChange: (next: Choice) => void;
  onClear: () => void;
  firstChip: RefObject<HTMLButtonElement | null>;
}) {
  const selectId = useId();
  const legendId = useId();

  const toggle = (genre: Genre) => {
    const has = choice.genres.includes(genre);
    onChange({ ...choice, genres: cleanGenres(has ? choice.genres.filter((g) => g !== genre) : [...choice.genres, genre]) });
  };

  return (
    <div className="for-you-controls">
      {models !== null && (
        <div className="field for-you-controls__model">
          <label htmlFor={selectId}>Model</label>
          <select
            id={selectId}
            value={choice.model}
            onChange={(event) => {
              const picked = models.models.find((m) => m === event.target.value);
              if (picked) onChange({ ...choice, model: picked });
            }}
          >
            {models.models.map((model) => (
              <option key={model} value={model}>
                {modelLabel(model)}
                {model === models.default_model ? ' (default)' : ''}
              </option>
            ))}
          </select>
        </div>
      )}
      <div className="genre-chips" role="group" aria-labelledby={legendId}>
        <p id={legendId} className="genre-chips__label">
          Categories
        </p>
        <div className="genre-chips__row">
          {CHIP_ORDER.map((genre, index) => (
            <button
              key={genre}
              ref={index === 0 ? firstChip : undefined}
              type="button"
              className="chip"
              aria-pressed={choice.genres.includes(genre)}
              onClick={() => toggle(genre)}
            >
              {genre}
            </button>
          ))}
          <button
            type="button"
            className="button button--quiet button--small"
            disabled={choice.genres.length === 0}
            onClick={onClear}
          >
            Clear
          </button>
        </div>
      </div>
    </div>
  );
}

function RecommendationList({
  model: requested,
  genresKey,
  genres,
  onClearGenres,
}: {
  model: ModelName;
  genresKey: string | undefined;
  genres: Genre[];
  onClearGenres: () => void;
}) {
  // An unknown_model answer (the model list was unavailable and knn is not loaded) falls back to pop.
  const load = useCallback(
    async (signal: AbortSignal) => {
      try {
        return await fetchRecommendations(requested, genresKey, signal);
      } catch (error) {
        if (!(error instanceof ApiError && error.code === 'unknown_model' && requested !== 'pop')) throw error;
        return fetchRecommendations('pop', genresKey, signal);
      }
    },
    [requested, genresKey],
  );

  const { state, reload } = useAsync(load);
  const ratings = useOptimisticRatings();
  const [hidden, setHidden] = useState<ReadonlySet<number>>(new Set());
  useRefetchAfterRating(() => reload({ quiet: true }));

  if (state.status === 'loading') return <Loading>Loading your recommendations…</Loading>;
  if (state.status === 'error') return <ErrorMessage message={state.message} onRetry={() => reload()} />;

  const { data } = state;
  const items = data.items.filter((item) => !hidden.has(item.movieId));
  const filtered = genres.length > 0;
  const phrase = genrePhrase(genres);

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
      <p className="for-you-summary">
        {`Picks from ${modelLabel(data.model)}`}
        {filtered && <span className="muted"> · {genres.join(', ')}</span>}
      </p>
      {data.cold_start && (
        <div className="notice">
          <p>These are popular picks while we get to know you. Rate at least 5 movies to get recommendations made for you.</p>
          <Link to="/onboarding" className="button button--primary">
            Rate more movies
          </Link>
        </div>
      )}
      {filtered && data.items.length > 0 && data.items.length < TOP_K && (
        <p className="notice">
          Only {data.items.length} {phrase} {data.items.length === 1 ? 'pick' : 'picks'} right now — rate more movies
          or widen the categories.
        </p>
      )}
      {items.length === 0 ? (
        <Empty>
          {filtered ? (
            <>
              <p>No {phrase} picks right now.</p>
              <p>
                <button type="button" className="button" onClick={onClearGenres}>
                  Clear categories
                </button>
              </p>
            </>
          ) : (
            <>
              <p>No recommendations right now.</p>
              <p>
                <Link to="/onboarding">Rate a few more movies</Link> and check back.
              </p>
            </>
          )}
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
