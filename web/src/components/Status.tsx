// Loading and error messages used by every page section.
import type { ReactNode } from 'react';

/** A polite live region, so screen readers hear that something is loading. */
export function Loading({ children }: { children: ReactNode }) {
  return (
    <p role="status" className="muted loading">
      {children}
    </p>
  );
}

/** An error with an optional "Try again" button. */
export function ErrorMessage({ message, onRetry }: { message: string; onRetry?: () => void }) {
  return (
    <div role="alert" className="form-error error-message">
      <p>{message}</p>
      {onRetry && (
        <button type="button" className="button" onClick={onRetry}>
          Try again
        </button>
      )}
    </div>
  );
}

/** A friendly "nothing here yet" note. */
export function Empty({ children }: { children: ReactNode }) {
  return <div className="empty-state">{children}</div>;
}
