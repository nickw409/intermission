// Confirmation dialog for deleting the account (DELETE /api/me). Deleting cannot be undone, so the
// button stays disabled until the user types their username exactly.
//
// Accessibility: a modal dialog (role="dialog", aria-modal) labelled by its heading and described
// by the explanation and retention note. Focus starts in the confirmation field, Tab and Shift+Tab
// stay inside the dialog, and Escape or Cancel closes it without a request. The caller returns
// focus to the button that opened it.
import { useEffect, useId, useRef, useState, type FormEvent, type KeyboardEvent } from 'react';
import { TextField } from '../components/TextField';
import { friendlyMessage } from '../lib/errors';

export const RETENTION_NOTE =
  'Nightly backups keep a copy for up to 5 weeks and training exports for up to 7 days before they are removed.';

interface DeleteAccountDialogProps {
  username: string;
  /** Performs the deletion; a rejection is shown in the dialog and the dialog stays open. */
  onConfirm: () => Promise<void>;
  onCancel: () => void;
}

const FOCUSABLE = 'button:not([disabled]), input:not([disabled]), a[href], [tabindex]:not([tabindex="-1"])';

export function DeleteAccountDialog({ username, onConfirm, onCancel }: DeleteAccountDialogProps) {
  const titleId = useId();
  const descriptionId = useId();
  const dialogRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const [typed, setTyped] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const matches = typed === username;

  useEffect(() => {
    inputRef.current?.focus();
  }, []);

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      // Stop here so the account menu around us stays open (focus goes back to its button).
      event.stopPropagation();
      if (!busy) onCancel();
      return;
    }
    if (event.key !== 'Tab' || !dialogRef.current) return;
    const focusable = [...dialogRef.current.querySelectorAll<HTMLElement>(FOCUSABLE)];
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (!first || !last) return;
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  };

  const onSubmit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!matches || busy) return;
    setBusy(true);
    setError(null);
    try {
      await onConfirm();
      // On success the caller unmounts this dialog; nothing more to do here.
    } catch (err) {
      setError(friendlyMessage(err));
      setBusy(false);
    }
  };

  return (
    <div className="modal-backdrop">
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        aria-describedby={descriptionId}
        className="modal"
        onKeyDown={onKeyDown}
      >
        <h2 id={titleId}>Delete your account?</h2>
        <div id={descriptionId}>
          <p>Your account and all your ratings are deleted immediately. This cannot be undone.</p>
          <p className="muted">{RETENTION_NOTE}</p>
        </div>
        <form className="form" noValidate onSubmit={(e) => void onSubmit(e)}>
          {error && (
            <p role="alert" className="form-error">
              {error}
            </p>
          )}
          <TextField
            ref={inputRef}
            label="Type your username to confirm"
            name="confirm-username"
            autoComplete="off"
            autoCapitalize="none"
            spellCheck={false}
            hint={`Your username is ${username}.`}
            value={typed}
            onChange={setTyped}
          />
          <div className="modal__actions">
            <button type="button" className="button" onClick={onCancel} disabled={busy}>
              Cancel
            </button>
            <button type="submit" className="button button--danger" disabled={!matches || busy}>
              Delete my account
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
