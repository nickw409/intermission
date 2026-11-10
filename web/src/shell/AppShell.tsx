import { useEffect, useState } from 'react';
import { NavLink, Outlet } from 'react-router';
import { api, ApiError } from '../api/client';

const NAV = [
  { to: '/', label: 'For you' },
  { to: '/compare', label: 'Compare' },
  { to: '/ratings', label: 'My ratings' },
  { to: '/evaluation', label: 'Evaluation' },
  { to: '/how-it-works', label: 'How it works' },
];

type Session = { state: 'loading' } | { state: 'anonymous' } | { state: 'user'; username: string };

export function AppShell() {
  const [session, setSession] = useState<Session>({ state: 'loading' });

  useEffect(() => {
    const controller = new AbortController();
    api
      .get('/api/me', { signal: controller.signal })
      .then((me) => setSession({ state: 'user', username: me.username }))
      .catch((err: unknown) => {
        if (err instanceof ApiError) setSession({ state: 'anonymous' });
      });
    return () => controller.abort();
  }, []);

  return (
    <div className="shell">
      <header className="shell-header">
        <NavLink to="/" className="brand">
          Intermission
        </NavLink>
        <nav aria-label="Main">
          {NAV.map((item) => (
            <NavLink key={item.to} to={item.to} end={item.to === '/'}>
              {item.label}
            </NavLink>
          ))}
        </nav>
        <div className="account">
          {session.state === 'user' && <span>{session.username}</span>}
          {session.state === 'anonymous' && <NavLink to="/login">Log in</NavLink>}
        </div>
      </header>
      <main>
        <Outlet />
      </main>
    </div>
  );
}
