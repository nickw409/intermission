// Package invites mints and redeems invite codes (16 random bytes, base32,
// stored as SHA-256). Codes are created by `recs-api admin invite create`;
// redemption is called from auth inside the signup transaction.
//
// Owner: workstream I1.
//
// Invalid, expired and used-up codes all return the same invalid_invite
// error, and the invite check runs before any username lookup (order frozen
// in contracts/public.yaml).
package invites
