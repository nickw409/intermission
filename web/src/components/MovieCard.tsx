// Poster card for a movie: poster, linked title, year, and whatever the page adds below
// (explanation, rating control, buttons).
import type { ReactNode } from 'react';
import { Link } from 'react-router';
import { displayTitle } from '../lib/format';
import { Poster } from './Poster';

interface MovieCardProps {
  movieId: number;
  title: string;
  year: number | null;
  /** Heading level of the title within the page outline. */
  level?: 2 | 3;
  children?: ReactNode;
}

export function MovieCard({ movieId, title, year, level = 2, children }: MovieCardProps) {
  const Heading = level === 2 ? 'h2' : 'h3';
  return (
    <article className="movie-card">
      <Poster movieId={movieId} title={title} year={year} />
      <div className="movie-card__body">
        <Heading className="movie-card__title">
          <Link to={`/movies/${movieId}`}>{displayTitle(title)}</Link>
        </Heading>
        {year !== null && <p className="movie-card__year muted">{year}</p>}
        {children}
      </div>
    </article>
  );
}
