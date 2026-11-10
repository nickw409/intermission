// Renders its children only for a signed-in user. Pages keep their <h1> outside this, so the
// heading is there from the first paint whatever the session state.
import type { ReactNode } from 'react';
import { Link } from 'react-router';
import { useSession, type Me } from '../auth/session';
import { ErrorMessage, Loading } from './Status';

interface RequireSessionProps {
  /** Completes "Log in to …", e.g. "see your recommendations". */
  action: string;
  children: (me: Me) => ReactNode;
}

export function RequireSession({ action, children }: RequireSessionProps) {
  const { session, retry } = useSession();
  switch (session.status) {
    case 'loading':
      return <Loading>Checking your sign-in…</Loading>;
    case 'error':
      return <ErrorMessage message="Couldn't check your sign-in." onRetry={retry} />;
    case 'anonymous':
      return (
        <p className="card sign-in-prompt">
          <Link to="/login">Log in to {action}</Link>, or <Link to="/signup">create an account</Link>.
        </p>
      );
    case 'user':
      return children(session.me);
  }
}
