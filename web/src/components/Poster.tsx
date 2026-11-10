// A movie poster from recs-api's same-origin proxy. The alt text is the title and year, so the
// image is meaningful on its own (spec: alt text on posters). The proxy always answers 200 with an
// image (a placeholder when TMDB has none), so there is no broken-image state to handle.
import { titleWithYear } from '../lib/format';
import { posterUrl } from '../lib/poster';

interface PosterProps {
  movieId: number;
  title: string;
  year: number | null;
  size?: 'small' | 'medium' | 'large';
}

export function Poster({ movieId, title, year, size = 'medium' }: PosterProps) {
  return (
    <img
      className={`poster poster--${size}`}
      src={posterUrl(movieId)}
      alt={titleWithYear(title, year)}
      width={342}
      height={513}
      loading="lazy"
      decoding="async"
    />
  );
}
