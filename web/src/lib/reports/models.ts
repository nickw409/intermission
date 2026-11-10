// How models and runs are named, ordered and styled on the Evaluation page.
//
// Colors: an 8-hue categorical palette checked for color-vision deficiency. Each known model owns
// one slot permanently ("color follows the entity"), so a model looks the same in every chart and
// keeps its color when other models are filtered out. Separate light and dark steps of the same hue
// are chosen for each theme; both sets were validated against this site's surfaces (#ffffff and
// #1e1a17): every color has at least 3:1 contrast with its surface, adjacent-slot CVD separation
// is >= 8.4 ΔE (OKLab x100) and normal-vision separation >= 18.4 ΔE. (The light orange, aqua,
// yellow and magenta are darker steps of the reference hues, chosen to reach 3:1 on white.)
// Six series cannot all be told apart pairwise by color alone (no palette can), so every chart
// also encodes the model by position, marker shape or line dash, with a legend and data table.

export interface ModelStyle {
  /** Stroke/fill on the light theme. */
  light: string;
  /** Stroke/fill on the dark theme. */
  dark: string;
  /** Observable Plot symbol name. */
  symbol: string;
  /** SVG stroke-dasharray ('' = solid). */
  dash: string;
}

/** Baselines first, then the learned models, in the order the project builds them. */
export const MODEL_ORDER = ['pop', 'pop_count', 'knn', 'content', 'mf', 'als'] as const;

/** The popularity baseline every model must beat (spec: otherwise the model is a failure). */
export const BASELINE_MODEL = 'pop';

const KNOWN: Record<string, ModelStyle> = {
  pop: { light: '#2a78d6', dark: '#3987e5', symbol: 'circle', dash: '6,3' },
  pop_count: { light: '#d95926', dark: '#d95926', symbol: 'square', dash: '2,3' },
  knn: { light: '#17a072', dark: '#199e70', symbol: 'triangle', dash: '8,3,2,3' },
  content: { light: '#bf8500', dark: '#c98500', symbol: 'diamond', dash: '12,4' },
  mf: { light: '#d6588c', dark: '#d55181', symbol: 'star', dash: '3,2,3,6' },
  als: { light: '#008300', dark: '#008300', symbol: 'wye', dash: '' },
};

// Models this page doesn't know yet (e.g. a later hybrid) take the two remaining slots, chosen by
// a hash of the name so the choice never depends on which other models are on screen.
const EXTRA: ModelStyle[] = [
  { light: '#6250d6', dark: '#9085e9', symbol: 'hexagon', dash: '1,3' },
  { light: '#e34948', dark: '#e66767', symbol: 'cross', dash: '10,2,2,2,2,2' },
];

function hash(text: string): number {
  let h = 0;
  for (const char of text) h = (h * 31 + char.charCodeAt(0)) >>> 0;
  return h;
}

export function modelStyle(model: string): ModelStyle {
  return KNOWN[model] ?? EXTRA[hash(model) % EXTRA.length]!;
}

/** Sort comparator: known models in MODEL_ORDER, then any others alphabetically. */
export function compareModels(a: string, b: string): number {
  const rank = (m: string) => {
    const i = (MODEL_ORDER as readonly string[]).indexOf(m);
    return i === -1 ? MODEL_ORDER.length : i;
  };
  return rank(a) - rank(b) || a.localeCompare(b);
}

function formatValue(value: unknown): string {
  return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean'
    ? String(value)
    : JSON.stringify(value);
}

/** `{k: 128, lambda: 0.05}` -> "k=128, lambda=0.05"; a key the config lacks reads "k unset". */
export function configLabel(config: object, keys: string[] = Object.keys(config).sort()): string {
  if (keys.length === 0) return 'default config';
  const values = config as Record<string, unknown>;
  return keys
    .map((key) => (Object.hasOwn(values, key) ? `${key}=${formatValue(values[key])}` : `${key} unset`))
    .join(', ');
}

/** "2026-11-09T22:02:10Z" -> "2026-11-09 22:02 UTC". */
export function formatRunTime(iso: string): string {
  const d = new Date(iso);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())} UTC`;
}

export interface LabelledRun {
  run_id: string;
  model: string;
  config: object;
  created_at: string;
}

/**
 * Short labels for runs, keyed by run_id. Runs of the same model are told apart by only the
 * config keys whose values differ between them ("als · k=128" vs "als · k=64"); if the configs
 * are identical, the run time is added.
 */
export function runLabels(runs: LabelledRun[]): Map<string, string> {
  const labels = new Map<string, string>();
  const byModel = new Map<string, LabelledRun[]>();
  for (const run of runs) byModel.set(run.model, [...(byModel.get(run.model) ?? []), run]);
  for (const [model, siblings] of byModel) {
    const allKeys = [...new Set(siblings.flatMap((run) => Object.keys(run.config)))].sort();
    const differing = allKeys.filter((key) => {
      const seen = new Set(siblings.map((run) => configLabel(run.config, [key])));
      return seen.size > 1;
    });
    for (const run of siblings) {
      if (siblings.length === 1) {
        labels.set(run.run_id, `${model} · ${configLabel(run.config)}`);
      } else if (differing.length > 0) {
        labels.set(run.run_id, `${model} · ${configLabel(run.config, differing)}`);
      } else {
        labels.set(run.run_id, `${model} · ${configLabel(run.config)} · run ${formatRunTime(run.created_at)}`);
      }
    }
  }
  return labels;
}

export interface Estimate {
  mean: number;
  ci95: number[];
}

/**
 * The spec's failure rule, using CI overlap only (the paired bootstrap isn't in the public
 * report): a model beats popularity only if its whole 95% CI lies above popularity's. A CI that
 * overlaps popularity's, or lies below it, means "does not beat popularity".
 */
export function popularityVerdict(model: Estimate, popularity: Estimate): 'beats' | 'fails' {
  const modelLow = Math.min(...model.ci95);
  const popHigh = Math.max(...popularity.ci95);
  return modelLow > popHigh ? 'beats' : 'fails';
}
