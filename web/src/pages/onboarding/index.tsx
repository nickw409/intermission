// Onboarding: rate about 30 popular, genre-diverse movies (GET /api/onboarding) or search for any
// movie, until there are 10 ratings. A progress bar counts every rating the user has, including
// ones made before (GET /api/ratings), and moves the moment a star is clicked.
import { useCallback, useId, useState } from 'react';
import { Link } from 'react-router';
import { api, type Schemas } from '../../api/client';
import { ONBOARDING_THRESHOLD, type Me } from '../../auth/session';
import { MovieCard } from '../../components/MovieCard';
import { RateMovie } from '../../components/RateMovie';
import { RequireSession } from '../../components/RequireSession';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { TextField } from '../../components/TextField';
import { useOptimisticRatings, type OptimisticRatings, type RatingValue } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import { useDebouncedValue } from '../../lib/useDebouncedValue';
import '../../components/components.css';

type MovieSummary = Schemas['MovieSummary'];

const SUGGESTIONS = 30;
const SEARCH_LIMIT = 12;
const SEARCH_DELAY_MS = 300;

export default function OnboardingPage() {
  return (
    <div className="page">
      <h1>Rate some movies</h1>
      <p className="lede">
        Rate at least {ONBOARDING_THRESHOLD} movies you've seen so the recommenders can learn your taste. Half stars are
        fine.
      </p>
      <RequireSession action="rate movies">{(me) => <Onboarding me={me} />}</RequireSession>
    </div>
  );
}

/** Rating lookups shared by the suggestion grid and the search results. */
interface Rater {
  valueOf: (movieId: number) => RatingValue;
  errorOf: (movieId: number) => string | undefined;
  rate: (movieId: number, next: RatingValue) => void;
}

function Onboarding({ me }: { me: Me }) {
  const ratings = useOptimisticRatings();
  const loadRated = useCallback((signal: AbortSignal) => api.get('/api/ratings', { signal }), []);
  const rated = useAsync(loadRated).state;
  const loadSuggestions = useCallback(
    (signal: AbortSignal) => api.get('/api/onboarding', { query: { n: SUGGESTIONS }, signal }),
    [],
  );
  const suggestions = useAsync(loadSuggestions);

  const known = new Map(rated.status === 'ready' ? rated.data.items.map((r) => [r.movieId, r.rating]) : []);
  const valueOf = (movieId: number) => ratings.ratingOf(movieId, known.get(movieId) ?? null);
  const rater: Rater = {
    valueOf,
    errorOf: ratings.errorOf,
    rate: (movieId, next) => void ratings.rate(movieId, next, valueOf(movieId)),
  };
  const count = countRatings(rated.status === 'ready' ? known : null, me.rating_count, ratings);

  return (
    <>
      <Progress count={count} />
      <Search rater={rater} />
      {suggestions.state.status === 'loading' && <Loading>Loading movies to rate…</Loading>}
      {suggestions.state.status === 'error' && (
        <ErrorMessage message={suggestions.state.message} onRetry={() => suggestions.reload()} />
      )}
      {suggestions.state.status === 'ready' &&
        (suggestions.state.data.items.length === 0 ? (
          <Empty>
            <p>You've rated everything we'd suggest. Search above for any other movie you've seen.</p>
          </Empty>
        ) : (
          <MovieGrid movies={suggestions.state.data.items} rater={rater} level={2} label="Movies to rate" />
        ))}
    </>
  );
}

/**
 * How many movies the user has rated. With the ratings list: the loaded set plus local changes.
 * Without it (still loading or failed): the account's count plus movies rated on this page.
 */
function countRatings(known: Map<number, number> | null, accountCount: number, ratings: OptimisticRatings): number {
  if (known === null) {
    let added = 0;
    for (const value of ratings.overrides.values()) if (value !== null) added += 1;
    return accountCount + added;
  }
  const ids = new Set(known.keys());
  for (const [movieId, value] of ratings.overrides) {
    if (value === null) ids.delete(movieId);
    else ids.add(movieId);
  }
  return ids.size;
}

function Progress({ count }: { count: number }) {
  const id = useId();
  const done = count >= ONBOARDING_THRESHOLD;
  const remaining = ONBOARDING_THRESHOLD - count;
  return (
    <div className="onboarding-progress card">
      <label htmlFor={id}>Ratings so far</label>
      <progress id={id} max={ONBOARDING_THRESHOLD} value={Math.min(count, ONBOARDING_THRESHOLD)} />
      <p aria-live="polite" className="onboarding-progress__count">
        {count <= ONBOARDING_THRESHOLD ? `${count} of ${ONBOARDING_THRESHOLD} rated` : `${count} rated`}
      </p>
      {done ? (
        <Link to="/" className="button button--primary">
          Continue
        </Link>
      ) : (
        <p className="muted">
          Rate {remaining} more {remaining === 1 ? 'movie' : 'movies'} for personal recommendations, or{' '}
          <Link to="/">skip for now</Link>.
        </p>
      )}
    </div>
  );
}

function Search({ rater }: { rater: Rater }) {
  const [query, setQuery] = useState('');
  const q = useDebouncedValue(query.trim(), SEARCH_DELAY_MS);
  return (
    <div className="onboarding-search">
      <TextField
        label="Search for any movie"
        type="search"
        value={query}
        onChange={setQuery}
        maxLength={100}
        autoComplete="off"
        hint="Type part of a title."
      />
      {q !== '' && <SearchResults key={q} q={q} rater={rater} />}
    </div>
  );
}

function SearchResults({ q, rater }: { q: string; rater: Rater }) {
  const headingId = useId();
  const load = useCallback(
    (signal: AbortSignal) => api.get('/api/movies/search', { query: { q, limit: SEARCH_LIMIT }, signal }),
    [q],
  );
  const { state, reload } = useAsync(load);
  return (
    <section className="search-results" aria-labelledby={headingId}>
      <h2 id={headingId}>Search results</h2>
      {state.status === 'loading' && <Loading>Searching…</Loading>}
      {state.status === 'error' && <ErrorMessage message={state.message} onRetry={() => reload()} />}
      {state.status === 'ready' &&
        (state.data.items.length === 0 ? (
          <p className="muted">No movies match “{q}”.</p>
        ) : (
          <MovieGrid movies={state.data.items} rater={rater} level={3} label={`Movies matching ${q}`} />
        ))}
    </section>
  );
}

function MovieGrid({
  movies,
  rater,
  level,
  label,
}: {
  movies: MovieSummary[];
  rater: Rater;
  level: 2 | 3;
  label: string;
}) {
  return (
    <ul className="card-grid" aria-label={label}>
      {movies.map((movie) => (
        <li key={movie.movieId}>
          <MovieCard movieId={movie.movieId} title={movie.title} year={movie.year} level={level}>
            {movie.genres.length > 0 && <p className="genres muted">{movie.genres.join(', ')}</p>}
            <RateMovie
              title={movie.title}
              value={rater.valueOf(movie.movieId)}
              onRate={(next) => rater.rate(movie.movieId, next)}
              error={rater.errorOf(movie.movieId)}
            />
          </MovieCard>
        </li>
      ))}
    </ul>
  );
}
