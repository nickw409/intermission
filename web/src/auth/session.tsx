// Who is logged in. The shell asks GET /api/me once on load; login and signup pages hand the
// returned Me straight to `signedIn`, `logOut` ends the session on the server, and
// `deleteAccount` deletes the user (DELETE /api/me) and then clears the session the same way.
//
// The session cookie is HttpOnly, so the browser code never sees it: /api/me is the only way to
// know whether a session exists.
//   - /api/me 200 → user; 401 → anonymous.
//   - Any other failure (5xx, network) → error: we don't know, so we don't claim "logged out";
//     the account menu offers a retry.
//   - A /api/me answer only counts while we are still waiting for it ('loading'), so a slow
//     response can't undo a login or logout that finished first.
//   - Any later call answering 401 `unauthorized` means the session expired → anonymous, unless
//     that request was sent before the most recent sign-in.
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { api, ApiError, onApiError, type Schemas } from '../api/client';

export type Me = Schemas['Me'];

export type Session =
  | { status: 'loading' }
  | { status: 'anonymous' }
  | { status: 'error' }
  | { status: 'user'; me: Me };

interface SessionContextValue {
  session: Session;
  signedIn: (me: Me) => void;
  logOut: () => Promise<void>;
  /**
   * Deletes the account. Resolves 'deleted' (204) or 'signed-out' (401: the session was already
   * gone); either way the session is cleared. Throws on any other failure and stays signed in.
   */
  deleteAccount: () => Promise<'deleted' | 'signed-out'>;
  /** Ask /api/me again (after an 'error'). */
  retry: () => void;
}

const SessionContext = createContext<SessionContextValue | null>(null);

// Requests whose 401s are handled where they are made, not by the expiry listener.
const SELF_HANDLED = new Set(['GET /api/me', 'POST /api/auth/logout', 'DELETE /api/me']);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [session, setSession] = useState<Session>({ status: 'loading' });
  const signedInAt = useRef(Number.NEGATIVE_INFINITY);

  const loadMe = useCallback((signal?: AbortSignal) => {
    const settle = (next: Session) => setSession((current) => (current.status === 'loading' ? next : current));
    api
      .get('/api/me', signal ? { signal } : {})
      .then((me) => settle({ status: 'user', me }))
      .catch((err: unknown) => {
        if (signal?.aborted) return;
        settle(err instanceof ApiError && err.status === 401 ? { status: 'anonymous' } : { status: 'error' });
      });
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    loadMe(controller.signal);
    return () => controller.abort();
  }, [loadMe]);

  useEffect(
    () =>
      onApiError((error) => {
        if (error.status !== 401 || error.code !== 'unauthorized' || !error.request) return;
        const { method, path, startedAt } = error.request;
        if (SELF_HANDLED.has(`${method} ${path}`)) return;
        if (startedAt < signedInAt.current) return; // sent before the current session existed
        setSession((current) => (current.status === 'user' ? { status: 'anonymous' } : current));
      }),
    [],
  );

  const signedIn = useCallback((me: Me) => {
    signedInAt.current = performance.now();
    setSession({ status: 'user', me });
  }, []);

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

  const deleteAccount = useCallback(async () => {
    let outcome: 'deleted' | 'signed-out' = 'deleted';
    try {
      await api.delete('/api/me');
    } catch (error) {
      if (!(error instanceof ApiError && error.status === 401)) throw error;
      outcome = 'signed-out';
    }
    setSession({ status: 'anonymous' });
    return outcome;
  }, []);

  const retry = useCallback(() => {
    setSession({ status: 'loading' });
    loadMe();
  }, [loadMe]);

  const value = useMemo(
    () => ({ session, signedIn, logOut, deleteAccount, retry }),
    [session, signedIn, logOut, deleteAccount, retry],
  );
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
