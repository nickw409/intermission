-- 0001_init: the schema from docs/spec.md ("Accounts, auth and persistence"),
-- plus users.ratings_version (bumped in the same transaction as every rating
-- write; it keys the recommendation cache). The extra indexes only speed up
-- lookups and ON DELETE CASCADE.

CREATE TABLE users (
  id               INTEGER PRIMARY KEY,
  username         TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(username) BETWEEN 3 AND 32),
  pw_hash          TEXT NOT NULL,              -- PHC string: $argon2id$v=19$m=65536,t=3,p=4$<salt>$<hash>
  created_at       INTEGER NOT NULL,
  ratings_version  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE sessions (
  token_hash  BLOB PRIMARY KEY,                -- SHA-256 of the cookie token; raw token never stored
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at  INTEGER NOT NULL,
  expires_at  INTEGER NOT NULL
);
CREATE INDEX sessions_user_id ON sessions(user_id);
CREATE INDEX sessions_expires_at ON sessions(expires_at);

CREATE TABLE ratings (
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  movie_id    INTEGER NOT NULL,                -- MovieLens movieId, validated against the loaded catalog
  rating      REAL NOT NULL CHECK (rating BETWEEN 0.5 AND 5.0 AND rating * 2 = CAST(rating * 2 AS INTEGER)),
  updated_at  INTEGER NOT NULL,
  PRIMARY KEY (user_id, movie_id)
);

CREATE TABLE invites (
  code_hash   BLOB PRIMARY KEY,                -- SHA-256 of the code; raw code shown once at creation
  max_uses    INTEGER NOT NULL DEFAULT 1,
  used_count  INTEGER NOT NULL DEFAULT 0,
  expires_at  INTEGER,                         -- NULL = never
  note        TEXT,                            -- who it was given to
  created_at  INTEGER NOT NULL
);

CREATE TABLE feedback (                        -- phase 6: online signals
  user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  movie_id  INTEGER NOT NULL,
  model     TEXT NOT NULL,
  action    TEXT NOT NULL CHECK (action IN ('impression','click','not_interested')),
  ts        INTEGER NOT NULL
);
CREATE INDEX feedback_user_id ON feedback(user_id);
