import '@testing-library/jest-dom/vitest';
import { cleanup, configure } from '@testing-library/react';
import { setupServer } from 'msw/node';
import { afterAll, afterEach, beforeAll } from 'vitest';
import { handlers } from '../mocks/handlers';

// findBy*/waitFor give up after 3 s instead of 1 s: whole-app renders can be slow when every test
// file runs in parallel on a busy machine. (The per-test timeout is unchanged.)
configure({ asyncUtilTimeout: 3000 });

export const server = setupServer(...handlers);

beforeAll(() => server.listen({ onUnhandledFrame: 'error' }));
afterEach(() => {
  cleanup();
  server.resetHandlers();
  server.events.removeAllListeners();
});
afterAll(() => server.close());
