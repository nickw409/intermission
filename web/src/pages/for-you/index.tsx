// For you: the signed-in user's top 20. The model defaults to the server's (GET /api/models
// default_model, falling back to knn, then pop), and the user can pick any loaded model and narrow
// the list with genre chips; the view lives in the URL and the user's choices are remembered per
// browser (see choice.ts). Each poster card explains itself ("Because you rated …"), can be rated in
// place, and can be dismissed with "Not interested". The list refetches 500 ms after a rating change
// is saved, so newly rated movies drop out and the rest re-rank.
import { useCallback, useRef, useState } from 'react';
import { useSearchParams } from 'react-router';
import { api } from '../../api/client';
import { RequireSession } from '../../components/RequireSession';
import { Loading } from '../../components/Status';
import { pickDefaultModel, type ModelName, type ModelsResponse } from '../../lib/models';
import { useAsync } from '../../lib/useAsync';
import { useDebouncedValue } from '../../lib/useDebouncedValue';
import { fromSearchParams, genresParam, readStored, resolveChoice, toSearchParams, writeStored, type Genre } from './choice';
import { GenreChips, ModelSelect } from './Controls';
import { RecommendationList } from './RecommendationList';
import '../../components/components.css';
import './for-you.css';

/** Chip toggles settle this long before the list is fetched (recommend calls are rate limited). */
const GENRE_DEBOUNCE_MS = 350;

export default function ForYouPage() {
  return (
    <div className="page">
      <h1 tabIndex={-1}>For you</h1>
      <RequireSession action="see your recommendations">{() => <Recommendations />}</RequireSession>
    </div>
  );
}

function Recommendations() {
  // Without a model list the page still works: no selector, and the default fallback chain.
  const loadModels = useCallback(
    (signal: AbortSignal) =>
      api.get('/api/models', { signal }).catch((error: unknown) => {
        if (signal.aborted) throw error;
        return null;
      }),
    [],
  );
  const { state } = useAsync(loadModels);
  if (state.status !== 'ready') return <Loading>Loading your recommendations…</Loading>;
  return <ForYou models={state.data} />;
}

function ForYou({ models }: { models: ModelsResponse | null }) {
  const [params, setParams] = useSearchParams();
  const [stored, setStored] = useState(readStored);
  const choice = resolveChoice(fromSearchParams(params), stored, models, pickDefaultModel(models));
  // The chips and URL change at once; the request waits for the toggling to stop.
  const fetchGenres = useDebouncedValue(genresParam(choice.genres), GENRE_DEBOUNCE_MS);

  const show = (model: ModelName, genres: Genre[]) =>
    // Without a model list the model is a guess, so it stays out of the URL.
    setParams(toSearchParams({ model, genres }, models !== null), { replace: true });
  const remember = (model: string | null, genres: Genre[]) => {
    setStored({ model, genres });
    writeStored({ model, genres });
  };
  const chooseModel = (model: ModelName) => {
    remember(model === models?.default_model ? null : model, choice.genres);
    show(model, choice.genres);
  };
  const chooseGenres = (genres: Genre[]) => {
    remember(stored.model, genres);
    show(choice.model, genres);
  };

  // Both Clear buttons become disabled or disappear once used; focus moves to the first chip.
  const firstChip = useRef<HTMLButtonElement>(null);
  const clearGenres = () => {
    firstChip.current?.focus();
    chooseGenres([]);
  };

  return (
    <>
      <div className="for-you-controls">
        {models !== null && <ModelSelect models={models} value={choice.model} onChange={chooseModel} />}
        <GenreChips selected={choice.genres} onChange={chooseGenres} onClear={clearGenres} firstChip={firstChip} />
      </div>
      <RecommendationList model={choice.model} genresKey={fetchGenres} onClearGenres={clearGenres} />
    </>
  );
}
