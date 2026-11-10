// Dev-only mock mode: `npm run dev:mock` (VITE_MOCK=1). Never part of the production bundle.
import { setupWorker } from 'msw/browser';
import { handlers } from './handlers';

export async function startMockWorker(): Promise<void> {
  await setupWorker(...handlers).start({ onUnhandledFrame: 'bypass' });
}
