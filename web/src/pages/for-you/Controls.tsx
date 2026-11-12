// For you controls: the model selector, the genre chips (toggle buttons with aria-pressed) and the
// release-year range (two number inputs).
import { useId, useState, type RefObject } from 'react';
import { modelLabel, type ModelName, type ModelsResponse } from '../../lib/models';
import {
  CHIP_ORDER,
  cleanGenres,
  hasYears,
  MAX_YEAR,
  MIN_YEAR,
  NO_YEARS,
  parseYear,
  type Genre,
  type YearRange,
} from './choice';

export function ModelSelect({
  models,
  value,
  onChange,
}: {
  models: ModelsResponse;
  value: ModelName;
  onChange: (model: ModelName) => void;
}) {
  const id = useId();
  return (
    <div className="field for-you-controls__model">
      <label htmlFor={id}>Model</label>
      <select
        id={id}
        value={value}
        onChange={(event) => {
          const picked = models.models.find((m) => m === event.target.value);
          if (picked) onChange(picked);
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
  );
}

export function GenreChips({
  selected,
  onChange,
  onClear,
  firstChip,
}: {
  selected: Genre[];
  onChange: (genres: Genre[]) => void;
  onClear: () => void;
  firstChip: RefObject<HTMLButtonElement | null>;
}) {
  const toggle = (genre: Genre) =>
    onChange(cleanGenres(selected.includes(genre) ? selected.filter((g) => g !== genre) : [...selected, genre]));
  return (
    <fieldset className="genre-chips">
      <legend className="genre-chips__label">Categories</legend>
      <div className="genre-chips__row">
        {CHIP_ORDER.map((genre, index) => (
          <button
            key={genre}
            ref={index === 0 ? firstChip : undefined}
            type="button"
            className="chip"
            aria-pressed={selected.includes(genre)}
            onClick={() => toggle(genre)}
          >
            {genre}
          </button>
        ))}
        <button
          type="button"
          className="button button--quiet button--small"
          disabled={selected.length === 0}
          onClick={onClear}
        >
          Clear categories
        </button>
      </div>
    </fieldset>
  );
}

/** The two year boxes as typed. Kept as text so a half-typed year ("19") survives re-renders. */
interface YearDraft {
  from: string;
  to: string;
}

const draftOf = (years: YearRange): YearDraft => ({ from: years.from?.toString() ?? '', to: years.to?.toString() ?? '' });

/** One box: '' is no bound (null), a valid year is that bound, anything else is not a year (yet). */
function readBox(text: string): number | null | undefined {
  if (text === '') return null;
  return parseYear(text) ?? undefined;
}

/**
 * "Released": From and To year inputs. A range is applied (onChange) only when each box is empty or
 * a whole year from 1800 to 2100 and From is not later than To; until then the last applied range
 * stays. A finished but unusable entry (out of range, or an inverted range) gets an inline message,
 * and leaving a box that does not hold a year puts the applied year back.
 */
export function YearRangeInputs({
  value,
  onChange,
  onClear,
  fromInput,
}: {
  value: YearRange;
  onChange: (years: YearRange) => void;
  onClear: () => void;
  fromInput: RefObject<HTMLInputElement | null>;
}) {
  const fromId = useId();
  const toId = useId();
  const messageId = useId();
  const [draft, setDraft] = useState(() => draftOf(value));

  // When the applied range changes from outside (Clear in the empty state, a link), show it, unless
  // the boxes already say it (the change came from typing here). This is React's "adjust state when
  // a prop changes" pattern: set state during render, guarded by the previous value.
  const [synced, setSynced] = useState(value);
  if (synced.from !== value.from || synced.to !== value.to) {
    setSynced(value);
    if (readBox(draft.from) !== value.from || readBox(draft.to) !== value.to) setDraft(draftOf(value));
  }

  const update = (side: keyof YearDraft, input: HTMLInputElement) => {
    // A number input reports '' for text it cannot read (such as "19e"); that is not "no bound".
    if (input.validity.badInput) return;
    const next = { ...draft, [side]: input.value };
    setDraft(next);
    const from = readBox(next.from);
    const to = readBox(next.to);
    if (from === undefined || to === undefined) return; // still typing, or not a year
    if (from !== null && to !== null && from > to) return; // inverted: explained below the inputs
    if (from !== value.from || to !== value.to) onChange({ from, to });
  };

  // Messages only for entries that look finished (4+ characters), not while a year is being typed.
  const outOfRange = (side: keyof YearDraft) => draft[side].length >= 4 && readBox(draft[side]) === undefined;
  const from = readBox(draft.from);
  const to = readBox(draft.to);
  const inverted = typeof from === 'number' && typeof to === 'number' && from > to;
  const invalid = { from: outOfRange('from') || inverted, to: outOfRange('to') || inverted };
  let message = '';
  if (outOfRange('from') || outOfRange('to')) message = `Enter a year from ${MIN_YEAR} to ${MAX_YEAR}.`;
  else if (inverted) message = '“From” must not be later than “To”.';

  // Leaving a box that holds something other than a year (half-typed, out of range) puts the applied
  // year back, so the boxes never disagree with the list for longer than the typing takes. An
  // inverted range is two real years, so it stays, with its message, until the user fixes it.
  const restore = (side: keyof YearDraft, input: HTMLInputElement) => {
    const applied = draftOf(value)[side];
    if (input.validity.badInput) input.value = applied; // text React never saw, such as "19e"
    if (readBox(draft[side]) === undefined) setDraft({ ...draft, [side]: applied });
  };

  const nothingToClear = !hasYears(value) && draft.from === '' && draft.to === '';
  const clear = () => {
    setDraft(draftOf(NO_YEARS));
    onClear();
  };

  const box = (side: keyof YearDraft, id: string, label: string) => (
    <div className="field year-range__field">
      <label htmlFor={id}>{label}</label>
      <input
        id={id}
        ref={side === 'from' ? fromInput : undefined}
        type="number"
        inputMode="numeric"
        min={MIN_YEAR}
        max={MAX_YEAR}
        placeholder="Any"
        value={draft[side]}
        aria-invalid={invalid[side] || undefined}
        aria-describedby={invalid[side] ? messageId : undefined}
        onChange={(event) => update(side, event.currentTarget)}
        onBlur={(event) => restore(side, event.currentTarget)}
      />
    </div>
  );

  return (
    <fieldset className="year-range">
      <legend className="year-range__label">Released</legend>
      <div className="year-range__row">
        {box('from', fromId, 'From')}
        {box('to', toId, 'To')}
        <button
          type="button"
          className="button button--quiet button--small"
          disabled={nothingToClear}
          onClick={clear}
        >
          Clear years
        </button>
      </div>
      {/* Always present (empty when all is well), so a message is announced when it appears. */}
      <p id={messageId} className="field__error year-range__message" aria-live="polite">
        {message}
      </p>
    </fieldset>
  );
}
