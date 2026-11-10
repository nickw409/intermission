export interface ModelStyle {
  light: string;
  dark: string;
  symbol: string;
  dash: string;
}

export function modelStyle(_model: string): ModelStyle {
  throw new Error('NotImplemented');
}

export function compareModels(_a: string, _b: string): number {
  throw new Error('NotImplemented');
}

export function configLabel(_config: object): string {
  throw new Error('NotImplemented');
}

export interface LabelledRun {
  run_id: string;
  model: string;
  config: object;
  created_at: string;
}

export function runLabels(_runs: LabelledRun[]): Map<string, string> {
  throw new Error('NotImplemented');
}

export interface Estimate {
  mean: number;
  ci95: number[];
}

export function popularityVerdict(_model: Estimate, _popularity: Estimate): 'beats' | 'fails' {
  throw new Error('NotImplemented');
}
