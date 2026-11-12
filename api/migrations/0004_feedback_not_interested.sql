-- 0004_feedback_not_interested: "not interested" is stored from now on
-- (POST /api/feedback). A user marks a movie at most once; this partial
-- unique index enforces that in the database and serves the per-user
-- lookups (the dedupe check, the 5,000 cap and the exclude list read by
-- recommend and onboarding) without scanning the user's other feedback.
-- No feedback rows were written before this release, so it cannot fail on
-- existing data. Works with the previous recs-api too (it never writes
-- feedback).
CREATE UNIQUE INDEX feedback_not_interested ON feedback(user_id, movie_id) WHERE action = 'not_interested';
