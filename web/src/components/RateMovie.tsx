// The signed-in user's rating control for one movie, with the reason a save failed (if it did).
import { displayTitle } from '../lib/format';
import type { RatingValue } from '../lib/ratings';
import { StarRating } from './StarRating';

interface RateMovieProps {
  title: string;
  value: RatingValue;
  onRate: (next: RatingValue) => void;
  error?: string | undefined;
}

export function RateMovie({ title, value, onRate, error }: RateMovieProps) {
  return (
    <div className="rate-movie">
      <StarRating label={`Your rating for ${displayTitle(title)}`} value={value} onChange={onRate} />
      {error && (
        <p role="alert" className="field__error">
          {error}
        </p>
      )}
    </div>
  );
}
