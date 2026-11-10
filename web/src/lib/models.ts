// Human names for the recommenders, and which one a page should use.
import type { Schemas } from '../api/client';

export type ModelName = Schemas['ModelName'];
export type ModelsResponse = Schemas['ModelsResponse'];

// Record<ModelName, …> makes a new model in the contract a type error until it has a name here.
const LABELS: Record<ModelName, string> = {
  pop: 'Popularity',
  pop_count: 'Popularity (count)',
  knn: 'Item-KNN',
  content: 'Content-based',
  mf: 'Matrix factorization',
  als: 'Implicit ALS',
  hybrid: 'Hybrid',
  mmr: 'Diversity re-ranker',
};

export function modelLabel(model: string): string {
  return Object.hasOwn(LABELS, model) ? LABELS[model as ModelName] : model;
}

/**
 * The model For you uses: the server's default if it is loaded, else knn, else pop. Without a model
 * list (the call failed) we try knn and let the page fall back to pop on unknown_model.
 */
export function pickDefaultModel(models: ModelsResponse | null): ModelName {
  if (!models) return 'knn';
  if (models.models.includes(models.default_model)) return models.default_model;
  if (models.models.includes('knn')) return 'knn';
  return 'pop';
}

/** Compare's first view: popularity, item-KNN and the default model, topped up to three. */
export function defaultComparison(models: ModelsResponse): ModelName[] {
  const wanted: ModelName[] = ['pop', 'knn', models.default_model, ...models.models];
  const picked: ModelName[] = [];
  for (const model of wanted) {
    if (models.models.includes(model) && !picked.includes(model)) picked.push(model);
    if (picked.length === 3) break;
  }
  // Keep the server's order so columns don't jump around as models are toggled.
  return models.models.filter((m) => picked.includes(m));
}
