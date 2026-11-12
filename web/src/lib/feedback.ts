// Online feedback on recommendations.
//
// "Not interested" is saved through POST /api/feedback: the movie is left out of the user's
// recommendations from then on, for every model. It can be undone (DELETE
// /api/feedback/not-interested/{movieId}), right away on For you or later on the Ratings page.
// (Impressions and clicks, the rest of phase 6's online signals, go through the same endpoint
// when that phase adds them.)
import { api, type Schemas } from '../api/client';

export interface NotInterested {
  movieId: number;
  model: Schemas['ModelName'];
}

export const feedbackHook = {
  /** Saves "not interested". Rejects with the ApiError if it could not be saved. */
  async notInterested(event: NotInterested): Promise<void> {
    await api.post('/api/feedback', { body: { movieId: event.movieId, model: event.model, action: 'not_interested' } });
  },
  /** Undoes "not interested" (a no-op if the movie was not marked). Rejects with the ApiError. */
  async undoNotInterested(movieId: number): Promise<void> {
    await api.delete('/api/feedback/not-interested/{movieId}', { path: { movieId } });
  },
};
