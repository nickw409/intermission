// Compare models: the same user's top 10 from three to five models side by side
// (GET /api/recommend/compare). Movies that more than one model picked are highlighted, and each
// one says in text which other lists it is in, so the overlap never depends on colour alone.
import { useCallback, useId, useState } from 'react';
import { Link } from 'react-router';
import { api, type Schemas } from '../../api/client';
import { Poster } from '../../components/Poster';
import { RequireSession } from '../../components/RequireSession';
import { ErrorMessage, Loading } from '../../components/Status';
import { displayTitle, joinTitles } from '../../lib/format';
import { defaultComparison, modelLabel, type ModelName, type ModelsResponse } from '../../lib/models';
import { useAsync } from '../../lib/useAsync';
import '../../components/components.css';

type CompareResponse = Schemas['CompareResponse'];

const PER_COLUMN = 10;
const MIN_MODELS = 3;
const MAX_MODELS = 5; // contracts/public.yaml: 1–5 models per compare call

export default function ComparePage() {
  return (
    <div className="page page--wide">
      <h1>Compare models</h1>
      <p className="lede">
        Each column is one recommender's top {PER_COLUMN} for you, from the same ratings. Where they agree, the movie is
        marked. See <Link to="/how-it-works">how each model works</Link>.
      </p>
      <RequireSession action="compare models">{() => <Compare />}</RequireSession>
    </div>
  );
}

function Compare() {
  const load = useCallback((signal: AbortSignal) => api.get('/api/models', { signal }), []);
  const { state, reload } = useAsync(load);
  if (state.status === 'loading') return <Loading>Loading models…</Loading>;
  if (state.status === 'error') return <ErrorMessage message={state.message} onRetry={() => reload()} />;
  return <Comparison models={state.data} />;
}

function Comparison({ models }: { models: ModelsResponse }) {
  const available = models.models;
  const [selected, setSelected] = useState<ModelName[]>(() => defaultComparison(models));
  const key = selected.join(',');
  const load = useCallback(
    (signal: AbortSignal) => api.get('/api/recommend/compare', { query: { models: key, k: PER_COLUMN }, signal }),
    [key],
  );
  const { state, reload } = useAsync(load);

  const toggle = (model: ModelName) =>
    setSelected((current) =>
      // Keep the server's order so columns don't jump around.
      available.filter((m) => (m === model ? !current.includes(m) : current.includes(m))),
    );

  return (
    <>
      <ModelPicker available={available} selected={selected} onToggle={toggle} />
      {state.status === 'loading' && <Loading>Loading recommendations…</Loading>}
      {state.status === 'error' && <ErrorMessage message={state.message} onRetry={() => reload()} />}
      {state.status === 'ready' && <Columns data={state.data} />}
    </>
  );
}

function ModelPicker({
  available,
  selected,
  onToggle,
}: {
  available: ModelName[];
  selected: ModelName[];
  onToggle: (model: ModelName) => void;
}) {
  const hintId = useId();
  const min = Math.min(MIN_MODELS, available.length);
  const max = Math.min(MAX_MODELS, available.length);
  return (
    <fieldset className="model-picker" aria-describedby={hintId}>
      <legend>Models to compare</legend>
      <p id={hintId} className="field__hint">
        Choose {min === max ? max : `${min} to ${max}`} models.
      </p>
      <div className="model-picker__options">
        {available.map((model) => {
          const checked = selected.includes(model);
          const locked = checked ? selected.length <= min : selected.length >= max;
          return (
            <label key={model} className="model-picker__option">
              <input
                type="checkbox"
                value={model}
                checked={checked}
                disabled={locked}
                onChange={() => onToggle(model)}
              />
              {modelLabel(model)}
            </label>
          );
        })}
      </div>
    </fieldset>
  );
}

function Columns({ data }: { data: CompareResponse }) {
  // movieId → the models whose list contains it.
  const listsFor = new Map<number, ModelName[]>();
  for (const result of data.results) {
    for (const item of result.items) listsFor.set(item.movieId, [...(listsFor.get(item.movieId) ?? []), result.model]);
  }
  const shared = [...listsFor.values()].filter((lists) => lists.length > 1).length;

  return (
    <>
      {data.cold_start && (
        <div className="notice">
          <p>
            With only a few ratings there is nothing for the models to differ on yet, so this is the popularity list. Rate
            at least 5 movies to compare models.
          </p>
          <Link to="/onboarding" className="button button--primary">
            Rate more movies
          </Link>
        </div>
      )}
      {/* In cold start the server sends a single popularity list, so there is no overlap to report. */}
      {!data.cold_start && (
        <p className="compare-summary">
          {shared === 0
          ? 'No movie appears in more than one list.'
          : `${shared} ${shared === 1 ? 'movie appears' : 'movies appear'} in more than one list.`}
        </p>
      )}
      <div className="compare-columns">
        {data.results.map((result) => (
          <Column key={result.model} model={result.model} items={result.items} listsFor={listsFor} />
        ))}
      </div>
    </>
  );
}

function Column({
  model,
  items,
  listsFor,
}: {
  model: ModelName;
  items: Schemas['RecItem'][];
  listsFor: Map<number, ModelName[]>;
}) {
  const headingId = useId();
  return (
    <section className="compare-column card" aria-labelledby={headingId}>
      <h2 id={headingId}>{modelLabel(model)}</h2>
      {items.length === 0 ? (
        <p className="muted">No movies from this model.</p>
      ) : (
        <ol className="compare-list">
          {items.map((item) => {
            const others = (listsFor.get(item.movieId) ?? []).filter((m) => m !== model);
            return (
              <li key={item.movieId} className={others.length > 0 ? 'compare-item compare-item--shared' : 'compare-item'}>
                <Poster movieId={item.movieId} title={item.title} year={item.year} size="small" />
                <div className="compare-item__body">
                  <Link to={`/movies/${item.movieId}`}>{displayTitle(item.title)}</Link>
                  {item.year !== null && <span className="muted"> {item.year}</span>}
                  {others.length > 0 && (
                    <p className="compare-item__shared">
                      <span aria-hidden="true">◆ </span>Also in {joinTitles(others.map(modelLabel))}
                    </p>
                  )}
                </div>
              </li>
            );
          })}
        </ol>
      )}
    </section>
  );
}
