import { useRef, useState, type FormEvent } from 'react';
import { Link, Navigate, useLocation, useNavigate } from 'react-router';
import { api } from '../../api/client';
import { homeFor, useSession } from '../../auth/session';
import { validateLogin, type FieldErrors, type LoginFields } from '../../auth/validation';
import { TextField } from '../../components/TextField';
import { friendlyMessage } from '../../lib/errors';

export default function LoginPage() {
  const navigate = useNavigate();
  // A one-line notice another page sent us here with (e.g. after deleting the account).
  const notice = noticeFrom(useLocation().state);
  const { session, signedIn } = useSession();
  const [fields, setFields] = useState<LoginFields>({ username: '', password: '' });
  const [errors, setErrors] = useState<FieldErrors<keyof LoginFields>>({});
  const [serverError, setServerError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const usernameRef = useRef<HTMLInputElement>(null);
  const passwordRef = useRef<HTMLInputElement>(null);

  const set = (name: keyof LoginFields) => (value: string) => setFields((f) => ({ ...f, [name]: value }));

  const onSubmit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const found = validateLogin(fields);
    setErrors(found);
    setServerError(null);
    if (found.username) return usernameRef.current?.focus();
    if (found.password) return passwordRef.current?.focus();

    setBusy(true);
    try {
      const me = await api.post('/api/auth/login', { body: fields });
      signedIn(me);
      void navigate(homeFor(me));
    } catch (err) {
      setServerError(friendlyMessage(err));
      setBusy(false);
    }
  };

  // Already signed in (e.g. a bookmarked /login): nothing to do here.
  if (session.status === 'user') return <Navigate to={homeFor(session.me)} replace />;

  return (
    <section className="auth-page">
      <h1>Log in</h1>
      {notice && (
        <p role="status" className="notice">
          {notice}
        </p>
      )}
      <form className="card form" noValidate onSubmit={(e) => void onSubmit(e)}>
        {serverError && (
          <p role="alert" className="form-error">
            {serverError}
          </p>
        )}
        <TextField
          ref={usernameRef}
          label="Username"
          name="username"
          autoComplete="username"
          autoCapitalize="none"
          spellCheck={false}
          value={fields.username}
          onChange={set('username')}
          error={errors.username}
        />
        <TextField
          ref={passwordRef}
          label="Password"
          name="password"
          type="password"
          autoComplete="current-password"
          value={fields.password}
          onChange={set('password')}
          error={errors.password}
        />
        <button type="submit" className="button button--primary" disabled={busy}>
          Log in
        </button>
      </form>
      <p className="auth-page__switch">
        New here? <Link to="/signup">Create an account</Link>
      </p>
    </section>
  );
}

function noticeFrom(state: unknown): string | null {
  if (state !== null && typeof state === 'object' && 'notice' in state && typeof state.notice === 'string') {
    return state.notice;
  }
  return null;
}
