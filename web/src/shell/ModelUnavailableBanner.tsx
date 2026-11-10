// Site-wide notice shown when any API call answers 503 `model_unavailable` (recs-model is down or
// timed out). The rest of the site keeps working: accounts, ratings and reports don't need the
// model service. Once dismissed it stays hidden until the page is reloaded, so pages that retry
// don't keep bringing it back.
import { useEffect, useState } from 'react';
import { onApiError } from '../api/client';

type BannerState = 'hidden' | 'shown' | 'dismissed';

export function ModelUnavailableBanner() {
  const [state, setState] = useState<BannerState>('hidden');

  useEffect(
    () =>
      onApiError((error) => {
        if (error.status === 503 && error.code === 'model_unavailable') {
          setState((current) => (current === 'hidden' ? 'shown' : current));
        }
      }),
    [],
  );

  // The live region stays mounted (empty when hidden) so screen readers announce the banner
  // when it appears.
  return (
    <div role="status">
      {state === 'shown' && (
        <div className="banner banner--warning">
          <p>Recommendations are temporarily unavailable. Everything else still works; please try again soon.</p>
          <button type="button" className="button button--quiet" onClick={() => setState('dismissed')}>
            Dismiss
          </button>
        </div>
      )}
    </div>
  );
}
