// For you controls: the model selector and the genre chips (toggle buttons with aria-pressed).
import { useId, type RefObject } from 'react';
import { modelLabel, type ModelName, type ModelsResponse } from '../../lib/models';
import { CHIP_ORDER, cleanGenres, type Genre } from './choice';

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
