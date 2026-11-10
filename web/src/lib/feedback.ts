// Online feedback on recommendations.
//
// PHASE 6 HOOK: "Not interested" currently only hides the card in the page. When phase 6 adds the
// feedback endpoint, make notInterested send it:
//
//   await api.post('/api/feedback', { body: { movieId, model, action: 'not_interested' } });
//
// Pages call feedbackHook.notInterested(...) already, so that is the only change needed.
import type { Schemas } from '../api/client';

export interface NotInterested {
  movieId: number;
  model: Schemas['ModelName'];
}

export const feedbackHook = {
  notInterested(event: NotInterested): void {
    // Intentionally nothing yet (see PHASE 6 HOOK above).
    void event;
  },
};
