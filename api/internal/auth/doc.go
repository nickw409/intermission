// Package auth implements accounts and sessions: signup (invite-checked in the
// same transaction that creates the user), login, logout, GET/DELETE /api/me,
// Argon2id password hashing behind a semaphore of 4, and session cookies.
//
// Owner: workstream I1.
//
// Signup check order is frozen in contracts/public.yaml: the invite code is
// validated before any username lookup, so an attacker without a valid code
// cannot probe which usernames exist.
package auth
