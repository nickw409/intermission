// Who is logged in. The shell asks GET /api/me once on load; login and signup pages hand the
// returned Me straight to `signedIn`, and `logOut` ends the session on the server.
//
// The session cookie is HttpOnly, so the browser code never sees it: /api/me is the only way to
// know whether a session exists. Any failure there (401, network error) means "logged out".
import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react';
import { api, ApiError, type Schemas } from '../api/client';

export type Me = Schemas['Me'];

export type Session = { status: 'loading' } | { status: 'anonymous' } | { status: 'user'; me: Me };

interface SessionContextValue {
  session: Session;
  signedIn: (me: Me) => void;
  logOut: () => Promise<void>;
}

const SessionContext = createContext<SessionContextValue | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [session, setSession] = useState<Session>({ status: 'loading' });

  useEffect(() => {
    const controller = new AbortController();
    api
      .get('/api/me', { signal: controller.signal })
      .then((me) => setSession({ status: 'user', me }))
      .catch(() => {
        if (!controller.signal.aborted) setSession({ status: 'anonymous' });
      });
    return () => controller.abort();
  }, []);

  const signedIn = useCallback((me: Me) => setSession({ status: 'user', me }), []);

  const logOut = useCallback(async () => {
    try {
      await api.post('/api/auth/logout');
    } catch (error) {
      // 401: the session was already gone, so the user is logged out anyway. Anything else means
      // the session may still be live; say so instead of pretending the logout worked.
      if (!(error instanceof ApiError && error.status === 401)) throw error;
    }
    setSession({ status: 'anonymous' });
  }, []);

  const value = useMemo(() => ({ session, signedIn, logOut }), [session, signedIn, logOut]);
  return <SessionContext value={value}>{children}</SessionContext>;
}

export function useSession(): SessionContextValue {
  const value = useContext(SessionContext);
  if (!value) throw new Error('useSession must be used inside <SessionProvider>');
  return value;
}

/** Where to go after logging in or signing up: onboarding until the user has 10 ratings. */
export const ONBOARDING_THRESHOLD = 10;
export function homeFor(me: Me): string {
  return me.rating_count < ONBOARDING_THRESHOLD ? '/onboarding' : '/';
}
