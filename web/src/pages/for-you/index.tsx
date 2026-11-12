// For you: the signed-in user's top 20. The model defaults to the server's (GET /api/models
// default_model, falling back to knn, then pop), and the user can pick any loaded model and narrow
// the list with genre chips and a range of release years; the view lives in the URL and the user's
// choices are remembered per browser (see choice.ts). Each poster card explains itself ("Because you
// rated …"), can be rated in place, and can be dismissed with "Not interested". The list refetches
// 500 ms after a rating change is saved, so newly rated movies drop out and the rest re-rank.
import { useCallback, useRef, useState } from 'react';
import { useSearchParams } from 'react-router';
import { api } from '../../api/client';
import { RequireSession } from '../../components/RequireSession';
import { Loading } from '../../components/Status';
import { pickDefaultModel, type ModelName, type ModelsResponse } from '../../lib/models';
import { useAsync } from '../../lib/useAsync';
import {
  filterParams,
  fromSearchParams,
  NO_YEARS,
  readStored,
  resolveChoice,
  toSearchParams,
  writeStored,
  type Choice,
  type Genre,
  type RawChoice,
  type YearRange,
} from './choice';
import { GenreChips, ModelSelect, YearRangeInputs } from './Controls';
import { RecommendationList } from './RecommendationList';
import { useSettledView } from './settledView';
import '../../components/components.css';
import './for-you.css';

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
  // The controls and URL change at once; the request waits for the chips and typing to settle. The
  // filter is debounced as one string (its query form) so a change to both sends one request, and a
  // model change sends the pending filter with it at once (useSettledView).
  const view = useSettledView(choice.model, filterParams(choice).toString());

  const show = (next: Choice) =>
    // Without a model list the model is a guess, so it stays out of the URL.
    setParams(toSearchParams(next, models !== null), { replace: true });
  const remember = (next: RawChoice) => {
    setStored(next);
    writeStored(next);
  };
  const chooseModel = (model: ModelName) => {
    remember({ ...choice, model: model === models?.default_model ? null : model });
    show({ ...choice, model });
  };
  const chooseGenres = (genres: Genre[]) => {
    remember({ ...choice, model: stored.model, genres });
    show({ ...choice, genres });
  };
  const chooseYears = (years: YearRange) => {
    remember({ ...choice, model: stored.model, years });
    show({ ...choice, years });
  };

  // Each Clear button becomes disabled or disappears once used, so focus moves into the control it
  // cleared: the first chip, or the From year.
  const firstChip = useRef<HTMLButtonElement>(null);
  const fromYear = useRef<HTMLInputElement>(null);
  const clearGenres = () => {
    firstChip.current?.focus();
    chooseGenres([]);
  };
  const clearYears = () => {
    fromYear.current?.focus();
    chooseYears(NO_YEARS);
  };

  return (
    <>
      <div className="for-you-controls">
        {models !== null && <ModelSelect models={models} value={choice.model} onChange={chooseModel} />}
        <GenreChips selected={choice.genres} onChange={chooseGenres} onClear={clearGenres} firstChip={firstChip} />
        <YearRangeInputs value={choice.years} onChange={chooseYears} onClear={clearYears} fromInput={fromYear} />
      </div>
      <RecommendationList
        model={view.model}
        filterKey={view.filterKey}
        onClearGenres={clearGenres}
        onClearYears={clearYears}
      />
    </>
  );
}
