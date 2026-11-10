// Package export implements `recs-api export-ratings`: writes website ratings
// to RECS_EXPORTS_DIR as CSV with internal user IDs only (no usernames) for
// the nightly retrain. The format is contracts/export.md.
//
// Account deletion and retention: a deleted account's ratings cascade away
// in app.db, so they are absent from every export written afterwards. They
// do remain in the older exports already on disk (at most Keep = 7 files)
// until those are pruned, i.e. for up to 7 further export runs (a week on a
// nightly timer). Those files hold only internal integer user IDs. Note that
// users.id has no AUTOINCREMENT, so SQLite may give the highest deleted ID to
// a new account; an older retained file can therefore attribute a deleted
// user's rows to that ID. recs-train reads only the file named by LATEST, so
// this never mixes accounts in training.
//
// Owner: workstream J.
package export
