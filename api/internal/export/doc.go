// Package export implements `recs-api export-ratings`: writes website ratings
// to RECS_EXPORTS_DIR as CSV with internal user IDs only (no usernames) for
// the nightly retrain. The format is contracts/export.md.
//
// Account deletion and retention: a deleted account's ratings cascade away
// in app.db, so they are absent from every export written afterwards. They
// do remain in the older exports already on disk until those are pruned:
// an export is deleted once it is beyond the Keep = 7 newest or MaxAge (7
// days) old, whichever comes first. The export LATEST names is kept whatever
// its age, because recs-train reads it: if exports stop, that one file can
// hold a deleted account's ratings until the next successful export. An
// export dated in the future (clock stepped back) is kept and warned about.
// Those files hold only internal integer user IDs, which are never reused
// (users.id is AUTOINCREMENT since migration 0003).
//
// Owner: workstream J.
package export
