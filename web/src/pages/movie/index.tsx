// Movie page: details (GET /api/movies/{id}), the plot summary, tagline, runtime, release date,
// directors and cast from TMDB (GET /api/movies/{id}/details, loaded alongside and optional: the
// page shows whatever arrived), the signed-in user's rating, and a row of similar movies with a
// toggle between item-KNN ("rated by the same people") and the content model
// (GET /api/movies/{id}/similar). TMDB text is rendered as plain text only.
import { useCallback, useId, useState } from 'react';
import { Link, useParams } from 'react-router';
import { api, ApiError, type Schemas } from '../../api/client';
import { useSession } from '../../auth/session';
import { MovieCard } from '../../components/MovieCard';
import { Poster } from '../../components/Poster';
import { RateMovie } from '../../components/RateMovie';
import { Empty, ErrorMessage, Loading } from '../../components/Status';
import { displayTitle, formatReleaseDate, formatRuntime } from '../../lib/format';
import { useOptimisticRatings } from '../../lib/ratings';
import { useAsync } from '../../lib/useAsync';
import '../../components/components.css';

type Movie = Schemas['Movie'];
type Details = Schemas['MovieDetails'];
type SimilarModel = Schemas['SimilarResponse']['model'];

const SIMILAR_COUNT = 12;
const MAX_MOVIE_ID = 2147483647;

/** The route's :movieId as a valid MovieLens id, or null. */
function parseMovieId(raw: string | undefined): number | null {
  if (!raw || !/^[1-9]\d{0,9}$/.test(raw)) return null;
  const id = Number(raw);
  return id <= MAX_MOVIE_ID ? id : null;
}

export default function MoviePage() {
  const id = parseMovieId(useParams().movieId);
  if (id === null) return <NotFound />;
  // A new id is a new page: start from a clean loading state.
  return <MovieDetails key={id} movieId={id} />;
}

function NotFound() {
  return (
    <div className="page">
      <h1>Movie not found</h1>
      <p>
        We couldn't find that movie. Try <Link to="/onboarding">searching for it</Link>.
      </p>
    </div>
  );
}

function MovieDetails({ movieId }: { movieId: number }) {
  const load = useCallback((signal: AbortSignal) => api.get('/api/movies/{movieId}', { path: { movieId }, signal }), [movieId]);
  const { state, reload } = useAsync(load);
  const { details, reload: reloadDetails } = useMovieDetails(movieId);

  if (state.status === 'error' && state.error instanceof ApiError && state.error.status === 404) return <NotFound />;
  if (state.status !== 'ready') {
    return (
      <div className="page">
        <h1 className="visually-hidden">Movie details</h1>
        {state.status === 'loading' ? (
          <Loading>Loading movie…</Loading>
        ) : (
          <ErrorMessage
            message={state.message}
            onRetry={() => {
              reload();
              if (!details) reloadDetails();
            }}
          />
        )}
      </div>
    );
  }

  const movie = state.data;
  return (
    <div className="page">
      <article className="movie-detail">
        <Poster movieId={movie.movieId} title={movie.title} year={movie.year} size="large" />
        <div className="movie-detail__body">
          <h1>{displayTitle(movie.title)}</h1>
          {details?.tagline && <p className="movie-detail__tagline">{details.tagline}</p>}
          <dl className="movie-facts">
            {movie.year !== null && (
              <>
                <dt>Year</dt>
                <dd>{movie.year}</dd>
              </>
            )}
            {movie.genres.length > 0 && (
              <>
                <dt>Genres</dt>
                <dd>{movie.genres.join(', ')}</dd>
              </>
            )}
            {details?.runtime_minutes != null && (
              <>
                <dt>Runtime</dt>
                <dd>{formatRuntime(details.runtime_minutes)}</dd>
              </>
            )}
            {details?.release_date && (
              <>
                <dt>Released</dt>
                <dd>{formatReleaseDate(details.release_date)}</dd>
              </>
            )}
            {details && details.directors.length > 0 && (
              <>
                <dt>{details.directors.length === 1 ? 'Director' : 'Directors'}</dt>
                <dd>{details.directors.join(', ')}</dd>
              </>
            )}
            <dt>Ratings</dt>
            <dd>
              {movie.mean_rating === null || movie.rating_count === 0
                ? 'No ratings yet'
                : `Average ${movie.mean_rating.toFixed(2)} from ${movie.rating_count} ${movie.rating_count === 1 ? 'rating' : 'ratings'}`}
            </dd>
          </dl>
          {details?.overview && <Overview text={details.overview} />}
          <YourRating movie={movie} />
        </div>
      </article>
      {details && details.cast.length > 0 && <CastList cast={details.cast} />}
      <SimilarRow movieId={movie.movieId} />
    </div>
  );
}

/**
 * TMDB details for the movie, or null while loading or if they couldn't be had. A failure is not
 * shown: the page already has the essentials, and recs-api answers empty details rather than an
 * error whenever TMDB is unavailable.
 */
function useMovieDetails(movieId: number): { details: Details | null; reload: () => void } {
  const load = useCallback(
    (signal: AbortSignal) => api.get('/api/movies/{movieId}/details', { path: { movieId }, signal }),
    [movieId],
  );
  const { state, reload } = useAsync(load);
  return { details: state.status === 'ready' ? state.data : null, reload };
}

function Overview({ text }: { text: string }) {
  const headingId = useId();
  return (
    <section className="movie-detail__overview" aria-labelledby={headingId}>
      <h2 id={headingId}>Plot</h2>
      <p>{text}</p>
    </section>
  );
}

function CastList({ cast }: { cast: Details['cast'] }) {
  const headingId = useId();
  return (
    <section className="cast" aria-labelledby={headingId}>
      <h2 id={headingId}>Cast</h2>
      <ul className="cast-list">
        {cast.map((member, i) => (
          <li key={`${i}-${member.name}`}>
            <span className="cast-list__name">{member.name}</span>
            {member.character && <span className="muted"> as {member.character}</span>}
          </li>
        ))}
      </ul>
    </section>
  );
}

function YourRating({ movie }: { movie: Movie }) {
  const { session } = useSession();
  const ratings = useOptimisticRatings();
  if (session.status === 'anonymous') {
    return (
      <p>
        <Link to="/login">Log in to rate this movie</Link>.
      </p>
    );
  }
  if (session.status !== 'user') return null;
  const value = ratings.ratingOf(movie.movieId, movie.user_rating);
  return (
    <div className="movie-detail__rating">
      <h2 className="movie-detail__rating-label">Your rating</h2>
      <RateMovie
        title={movie.title}
        value={value}
        onRate={(next) => void ratings.rate(movie.movieId, next, value)}
        error={ratings.errorOf(movie.movieId)}
      />
    </div>
  );
}

const SIMILAR_MODELS: { model: SimilarModel; label: string }[] = [
  { model: 'knn', label: 'Rated by the same people' },
  { model: 'content', label: 'Similar content' },
];

function SimilarRow({ movieId }: { movieId: number }) {
  const headingId = useId();
  const [model, setModel] = useState<SimilarModel>('knn');
  return (
    <section className="similar" aria-labelledby={headingId}>
      <h2 id={headingId}>Similar movies</h2>
      <div className="toggle" role="group" aria-label="Find similar movies by">
        {SIMILAR_MODELS.map((option) => (
          <button
            key={option.model}
            type="button"
            className="toggle__button"
            aria-pressed={model === option.model}
            onClick={() => setModel(option.model)}
          >
            {option.label}
          </button>
        ))}
      </div>
      <SimilarList key={model} movieId={movieId} model={model} />
    </section>
  );
}

function SimilarList({ movieId, model }: { movieId: number; model: SimilarModel }) {
  const load = useCallback(
    (signal: AbortSignal) =>
      api.get('/api/movies/{movieId}/similar', { path: { movieId }, query: { model, k: SIMILAR_COUNT }, signal }),
    [movieId, model],
  );
  const { state, reload } = useAsync(load);
  if (state.status === 'loading') return <Loading>Loading similar movies…</Loading>;
  if (state.status === 'error') return <ErrorMessage message={state.message} onRetry={() => reload()} />;
  if (state.data.items.length === 0) {
    return (
      <Empty>
        <p>No similar movies found.</p>
      </Empty>
    );
  }
  return (
    <ul className="card-row" aria-label={`Similar movies, ${model === 'knn' ? 'by ratings' : 'by content'}`}>
      {state.data.items.map((item) => (
        <li key={item.movieId}>
          <MovieCard movieId={item.movieId} title={item.title} year={item.year} level={3}>
            <p className="muted similarity">Similarity {item.similarity.toFixed(2)}</p>
          </MovieCard>
        </li>
      ))}
    </ul>
  );
}
