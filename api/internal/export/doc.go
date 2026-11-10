// Package export implements `recs-api export-ratings`: writes website ratings
// to RECS_EXPORTS_DIR as CSV with internal user IDs only (no usernames) for
// the nightly retrain. The format is contracts/export.md.
//
// Account deletion and retention: a deleted account's ratings cascade away
// in app.db, so they are absent from every export written afterwards. They
// do remain in the older exports already on disk until those are pruned:
// an export is deleted once it is beyond the Keep = 7 newest or older than
// MaxAge (7 days), whichever comes first, except the newest one. Those files
// hold only internal integer user IDs, which are never reused
// (users.id is AUTOINCREMENT since migration 0003). recs-train reads only
// the file named by LATEST.
//
// Owner: workstream J.
package export
