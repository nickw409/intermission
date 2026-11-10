import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { msw } from 'msw/vite';

export default defineConfig(({ command }) => ({
  // The MSW worker script is only served by the dev server (VITE_MOCK=1); never emitted to dist.
  plugins: [react(), ...(command === 'serve' ? [msw({ mode: 'worker-only' })] : [])],
  server: {
    // Mocks import the shared fixtures from ../contracts.
    fs: { allow: ['..'] },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
  },
}));
