// Package feedback serves POST /api/feedback (spec: Phase 6 online signals).
//
// "not_interested" is stored now: the movie is kept out of the caller's
// recommendations from then on (recommend reads these rows into the exclude
// list), and the write bumps users.ratings_version so cached lists are
// recomputed. A user keeps at most MaxNotInterested of them; past that the
// oldest is dropped. Doing it again for the same movie changes nothing.
//
// "impression" and "click" are accepted and validated but not stored yet:
// logging them and the click-through stats (GET /api/feedback/stats) are the
// rest of phase 6.
package feedback
