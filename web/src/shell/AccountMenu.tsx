// Right-hand side of the header: log in / sign up links, or the signed-in user's menu.
// The menu is a disclosure (a button that shows and hides a panel), which keeps keyboard use
// simple: Tab moves through the panel and Escape closes it.
import { useEffect, useId, useRef, useState } from 'react';
import { NavLink, useNavigate } from 'react-router';
import { useSession } from '../auth/session';
import { friendlyMessage } from '../lib/errors';

export function AccountMenu() {
  const { session, logOut, retry } = useSession();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const panelId = useId();
  const rootRef = useRef<HTMLDivElement>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    document.addEventListener('pointerdown', onPointerDown);
    return () => document.removeEventListener('pointerdown', onPointerDown);
  }, [open]);

  if (session.status === 'loading') return <div className="account" />;

  if (session.status === 'error') {
    return (
      <div className="account">
        <span className="muted">Couldn't check your sign-in.</span>
        <button type="button" className="button button--quiet" onClick={retry}>
          Retry
        </button>
      </div>
    );
  }

  if (session.status === 'anonymous') {
    return (
      <div className="account">
        <NavLink to="/login">Log in</NavLink>
        <NavLink to="/signup" className="button button--primary">
          Sign up
        </NavLink>
      </div>
    );
  }

  const { me } = session;

  const onLogOut = async () => {
    setBusy(true);
    setError(null);
    try {
      await logOut();
      setOpen(false);
      void navigate('/login');
    } catch (err) {
      setError(friendlyMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div
      className="account"
      ref={rootRef}
      onKeyDown={(event) => {
        if (event.key === 'Escape' && open) {
          setOpen(false);
          buttonRef.current?.focus();
        }
      }}
    >
      <button
        ref={buttonRef}
        type="button"
        className="account__button"
        aria-expanded={open}
        aria-controls={panelId}
        onClick={() => setOpen((v) => !v)}
      >
        <span>{me.username}</span>
        <span aria-hidden="true" className="account__caret">
          ▾
        </span>
      </button>
      {open && (
        <div id={panelId} className="account__panel">
          <p className="muted">
            {me.rating_count} {me.rating_count === 1 ? 'rating' : 'ratings'}
          </p>
          <button type="button" className="button" onClick={() => void onLogOut()} disabled={busy}>
            Log out
          </button>
          {error && (
            <p role="alert" className="form-error">
              {error}
            </p>
          )}
        </div>
      )}
    </div>
  );
}
