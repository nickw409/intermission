// Star rating in 0.5 steps (0.5–5.0), matching the ratings the API accepts.
//
// Interactive mode is one focusable element with role="slider" (WAI-ARIA slider pattern). The
// slider's range is 0–5, where 0 means "not rated" (value text "Not rated"); a slider must always
// have aria-valuenow, so "no rating" needs a position of its own.
//   ArrowRight/ArrowUp  +0.5        ArrowLeft/ArrowDown  -0.5 (not below 0.5)
//   Home  0.5                       End  5
//   Enter/Space  save the chosen value
//   Delete/Backspace  clear the rating
// Arrow keys only move a draft value; nothing is saved until Enter/Space, and leaving the control
// discards the draft. A click saves the half star under the pointer straight away: the whole
// row of stars is a single pointer target (at least 24px tall, WCAG 2.5.8) rather than ten tiny
// ones. A "Clear …" button is shown while a rating exists, so clearing also works with a pointer.
//
// Read-only mode renders a non-focusable role="img" with the value in its accessible name.
import { useId, useRef, useState, type KeyboardEvent, type MouseEvent } from 'react';

export const MIN_RATING = 0.5;
export const MAX_RATING = 5;
const STEP = 0.5;
const NOT_RATED = 0;

export function describeRating(value: number | null): string {
  if (value === null) return 'Not rated';
  return `${value} ${value === 1 ? 'star' : 'stars'}`;
}

function clamp(value: number): number {
  return Math.min(MAX_RATING, Math.max(MIN_RATING, value));
}

/** The half star under a click: the n-th half step from the left edge, rounded up. */
function valueAt(event: MouseEvent<HTMLElement>): number | null {
  const rect = event.currentTarget.getBoundingClientRect();
  if (rect.width <= 0) return null;
  const fraction = (event.clientX - rect.left) / rect.width;
  return clamp(Math.ceil(fraction * (MAX_RATING / STEP)) * STEP);
}

interface StarRatingProps {
  /** Accessible name, e.g. "Your rating for Spirited Away". */
  label: string;
  value: number | null;
  /** Called with the saved value, or null when the rating is cleared. */
  onChange?: (value: number | null) => void;
  readOnly?: boolean;
  /** "small" is for compact read-only display; interactive controls keep 24px targets either way. */
  size?: 'small' | 'medium';
}

export function StarRating({ label, value, onChange, readOnly = false, size = 'medium' }: StarRatingProps) {
  // `undefined` = no draft; `null` is a real (cleared) value.
  const [draft, setDraft] = useState<number | null | undefined>(undefined);
  const hintId = useId();
  const sliderRef = useRef<HTMLSpanElement>(null);
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
    if (next !== value) onChange?.(next);
  };

  const onKeyDown = (event: KeyboardEvent<HTMLSpanElement>) => {
    const from = shown ?? NOT_RATED; // first step up from "not rated" lands on 0.5
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
        commit(shown);
        return;
      case 'Delete':
      case 'Backspace':
        event.preventDefault();
        commit(null); // no-op (draft discarded) when there is no saved rating
        return;
      default:
        return;
    }
    event.preventDefault();
    setDraft(next);
  };

  const onClick = (event: MouseEvent<HTMLSpanElement>) => {
    const picked = valueAt(event);
    if (picked !== null) commit(picked);
  };

  return (
    <span className={className}>
      <span
        ref={sliderRef}
        role="slider"
        tabIndex={0}
        className="star-rating__control"
        aria-label={label}
        aria-valuemin={NOT_RATED}
        aria-valuemax={MAX_RATING}
        aria-valuenow={shown ?? NOT_RATED}
        aria-valuetext={describeRating(shown)}
        aria-describedby={hintId}
        onKeyDown={onKeyDown}
        onClick={onClick}
        onBlur={() => setDraft(undefined)}
      >
        <Stars value={shown} />
      </span>
      <span id={hintId} className="visually-hidden">
        Arrow keys choose, Enter saves, Delete clears.
      </span>
      {value !== null && (
        <button
          type="button"
          className="star-rating__clear"
          aria-label={`Clear ${label}`}
          onClick={() => {
            commit(null);
            sliderRef.current?.focus(); // the button disappears; keep focus on the control
          }}
        >
          <span aria-hidden="true">×</span>
        </button>
      )}
    </span>
  );
}

/** Five stars drawn as an empty row with a filled row clipped to the value's width on top. */
function Stars({ value }: { value: number | null }) {
  const percent = ((value ?? 0) / MAX_RATING) * 100;
  return (
    <span className="stars" aria-hidden="true">
      <span className="stars__empty">★★★★★</span>
      <span className="stars__filled" style={{ width: `${percent}%` }}>
        ★★★★★
      </span>
    </span>
  );
}
