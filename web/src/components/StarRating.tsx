// Star rating in 0.5 steps (0.5–5.0), matching the ratings the API accepts.
//
// Interactive mode is one focusable element with role="slider" (WAI-ARIA slider pattern):
//   ArrowRight/ArrowUp  +0.5        ArrowLeft/ArrowDown  -0.5
//   Home  0.5                       End  5
//   Enter/Space  save the chosen value
//   Delete/Backspace  clear the rating
// Arrow keys only move a draft value; nothing is saved until Enter/Space, and leaving the control
// discards the draft. Clicking a half star saves it straight away. A "Clear rating" button is shown
// while a rating exists, so clearing also works with a pointer.
//
// Read-only mode renders a non-focusable role="img" with the value in its accessible name.
import { useId, useState, type KeyboardEvent } from 'react';

export const MIN_RATING = 0.5;
export const MAX_RATING = 5;
const STEP = 0.5;

const HALF_STEPS = Array.from({ length: MAX_RATING / STEP }, (_, i) => (i + 1) * STEP); // 0.5 … 5

export function describeRating(value: number | null): string {
  if (value === null) return 'Not rated';
  return `${value} ${value === 1 ? 'star' : 'stars'}`;
}

function clamp(value: number): number {
  return Math.min(MAX_RATING, Math.max(MIN_RATING, value));
}

interface StarRatingProps {
  /** Accessible name, e.g. "Your rating for Spirited Away". */
  label: string;
  value: number | null;
  /** Called with the saved value, or null when the rating is cleared. */
  onChange?: (value: number | null) => void;
  readOnly?: boolean;
  size?: 'small' | 'medium';
}

export function StarRating({ label, value, onChange, readOnly = false, size = 'medium' }: StarRatingProps) {
  // `undefined` = no draft; `null` is a real (cleared) value.
  const [draft, setDraft] = useState<number | null | undefined>(undefined);
  const hintId = useId();
  const shown = draft === undefined ? value : draft;
  const className = `star-rating star-rating--${size}`;

  if (readOnly) {
    return (
      <span className={className} role="img" aria-label={`${label}: ${describeRating(value)}`}>
        <Stars value={value} />
      </span>
    );
  }

  const commit = (next: number | null) => {
    setDraft(undefined);
    onChange?.(next);
  };

  const onKeyDown = (event: KeyboardEvent<HTMLSpanElement>) => {
    const from = shown ?? 0; // first step up from "not rated" lands on 0.5
    let next: number | undefined;
    switch (event.key) {
      case 'ArrowRight':
      case 'ArrowUp':
        next = clamp(from + STEP);
        break;
      case 'ArrowLeft':
      case 'ArrowDown':
        next = clamp(from - STEP);
        break;
      case 'Home':
        next = MIN_RATING;
        break;
      case 'End':
        next = MAX_RATING;
        break;
      case 'Enter':
      case ' ':
        event.preventDefault();
        if (shown !== value) commit(shown);
        else setDraft(undefined);
        return;
      case 'Delete':
      case 'Backspace':
        event.preventDefault();
        commit(null);
        return;
      default:
        return;
    }
    event.preventDefault();
    setDraft(next);
  };

  return (
    <span className={className}>
      <span
        role="slider"
        tabIndex={0}
        className="star-rating__control"
        aria-label={label}
        aria-valuemin={MIN_RATING}
        aria-valuemax={MAX_RATING}
        aria-valuenow={shown ?? undefined}
        aria-valuetext={describeRating(shown)}
        aria-describedby={hintId}
        onKeyDown={onKeyDown}
        onBlur={() => setDraft(undefined)}
      >
        <Stars value={shown} onPick={commit} />
      </span>
      <span id={hintId} className="visually-hidden">
        Arrow keys choose, Enter saves, Delete clears.
      </span>
      {value !== null && (
        <button type="button" className="star-rating__clear" onClick={() => commit(null)}>
          <span aria-hidden="true">×</span>
          <span className="visually-hidden">Clear rating</span>
        </button>
      )}
    </span>
  );
}

/** Five stars drawn as an empty row with a filled row clipped to the value's width on top. */
function Stars({ value, onPick }: { value: number | null; onPick?: (value: number) => void }) {
  const percent = ((value ?? 0) / MAX_RATING) * 100;
  return (
    <span className="stars" aria-hidden="true">
      <span className="stars__empty">★★★★★</span>
      <span className="stars__filled" style={{ width: `${percent}%` }}>
        ★★★★★
      </span>
      {onPick && (
        <span className="stars__targets">
          {HALF_STEPS.map((step) => (
            <span key={step} data-star-value={step} onClick={() => onPick(step)} />
          ))}
        </span>
      )}
    </span>
  );
}
