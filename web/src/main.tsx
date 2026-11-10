import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { createBrowserRouter, RouterProvider } from 'react-router';
import { routes } from './routes';
import './styles.css';

async function start() {
  if (import.meta.env.DEV && import.meta.env.VITE_MOCK === '1') {
    const { startMockWorker } = await import('./mocks/browser');
    await startMockWorker();
  }
  const root = document.getElementById('root');
  if (!root) throw new Error('#root is missing from index.html');
  createRoot(root).render(
    <StrictMode>
      <RouterProvider router={createBrowserRouter(routes)} />
    </StrictMode>,
  );
}

void start();
