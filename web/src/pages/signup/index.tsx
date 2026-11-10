import { useRef, useState, type FormEvent } from 'react';
import { Link, useNavigate } from 'react-router';
import { api, type Schemas } from '../../api/client';
import { homeFor, useSession } from '../../auth/session';
import {
  PASSWORD_RULE,
  USERNAME_RULE,
  validateSignup,
  type FieldErrors,
  type SignupFields,
} from '../../auth/validation';
import { TextField } from '../../components/TextField';
import { friendlyMessage } from '../../lib/errors';

// The client can't know the server's signup_mode, so the invite field is always shown and the
// server decides whether it is needed.
const INVITE_HINT = 'Required while signup is invite-only';

export default function SignupPage() {
  const navigate = useNavigate();
  const { signedIn } = useSession();
  const [fields, setFields] = useState<SignupFields>({ username: '', password: '', invite: '' });
  const [errors, setErrors] = useState<FieldErrors<keyof SignupFields>>({});
  const [serverError, setServerError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const usernameRef = useRef<HTMLInputElement>(null);
  const passwordRef = useRef<HTMLInputElement>(null);
  const inviteRef = useRef<HTMLInputElement>(null);

  const set = (name: keyof SignupFields) => (value: string) => setFields((f) => ({ ...f, [name]: value }));

  const onSubmit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const found = validateSignup(fields);
    setErrors(found);
    setServerError(null);
    if (found.username) return usernameRef.current?.focus();
    if (found.password) return passwordRef.current?.focus();
    if (found.invite) return inviteRef.current?.focus();

    const body: Schemas['SignupRequest'] = { username: fields.username, password: fields.password };
    const invite = fields.invite.trim();
    if (invite) body.invite_code = invite;

    setBusy(true);
    try {
      const me = await api.post('/api/auth/signup', { body });
      signedIn(me);
      void navigate(homeFor(me));
    } catch (err) {
      setServerError(friendlyMessage(err));
      setBusy(false);
    }
  };

  return (
    <section className="auth-page">
      <h1>Sign up</h1>
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
          hint={USERNAME_RULE}
          error={errors.username}
        />
        <TextField
          ref={passwordRef}
          label="Password"
          name="password"
          type="password"
          autoComplete="new-password"
          value={fields.password}
          onChange={set('password')}
          hint={PASSWORD_RULE}
          error={errors.password}
        />
        <TextField
          ref={inviteRef}
          label="Invite code"
          name="invite_code"
          autoComplete="off"
          autoCapitalize="none"
          spellCheck={false}
          value={fields.invite}
          onChange={set('invite')}
          hint={INVITE_HINT}
          error={errors.invite}
        />
        <button type="submit" className="button button--primary" disabled={busy}>
          Sign up
        </button>
      </form>
      <p className="auth-page__switch">
        Already have an account? <Link to="/login">Log in to your account</Link>
      </p>
    </section>
  );
}
