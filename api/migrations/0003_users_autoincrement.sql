-- 0003_users_autoincrement: user IDs are never reused.
--
-- With a plain INTEGER PRIMARY KEY, SQLite hands a new row max(id) + 1, so
-- deleting the newest account and signing up again reuses its ID. Anything
-- keyed by user ID outside this table (recs-api's in-memory recommendation
-- cache, rate-limit buckets, exported files) could then be attributed to the
-- wrong person (security audit SA1-1). AUTOINCREMENT makes SQLite remember
-- the largest ID ever handed out (in sqlite_sequence) and never go below it.
--
-- SQLite cannot add AUTOINCREMENT to an existing column, so the table is
-- rebuilt with the documented procedure
-- (https://www.sqlite.org/lang_altertable.html#otheralter). The runner
-- (internal/db/migrate.go) switches foreign key enforcement off around the
-- transaction, so dropping the old table does not cascade into sessions,
-- ratings and feedback, and runs foreign_key_check before committing. Those
-- tables reference users by name, so after the rename they point at the new
-- table with ON DELETE CASCADE unchanged. users has no indexes besides the
-- UNIQUE username one (recreated by the constraint), no triggers and no views.
--
-- Copying explicit IDs seeds sqlite_sequence with the current max(id). An ID
-- deleted above that before this migration can be handed out once more; the
-- recommendation cache that made reuse harmful lives in memory and starts
-- empty after the restart that applies this migration.

CREATE TABLE users_new (
  id               INTEGER PRIMARY KEY AUTOINCREMENT,
  username         TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(username) BETWEEN 3 AND 32),
  pw_hash          TEXT NOT NULL,              -- PHC string: $argon2id$v=19$m=65536,t=3,p=4$<salt>$<hash>
  created_at       INTEGER NOT NULL,
  ratings_version  INTEGER NOT NULL DEFAULT 0
);

INSERT INTO users_new (id, username, pw_hash, created_at, ratings_version)
  SELECT id, username, pw_hash, created_at, ratings_version FROM users;

DROP TABLE users;

ALTER TABLE users_new RENAME TO users;
