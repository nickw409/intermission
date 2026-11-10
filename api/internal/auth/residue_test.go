package auth

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
)

// Security audit SA5-3: after DELETE /api/me, no byte of the account's
// ratings, username or password hash remains in app.db or app.db-wal.
// (Adapted from the SA5 auditor's probe.)

// residueTS is a rating timestamp no real row has; SQLite stores it as a
// 6-byte big-endian integer, so its bytes mark this account's rating rows.
const residueTS = 0x0A1B2C3D4E5F

var residueTSBytes = []byte{0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F}

type residueAccount struct {
	token    string
	username string
	hash     string
}

// newResidueAccount signs up username and gives it n ratings carrying the
// canary timestamp, then checkpoints so they sit in the main file.
func newResidueAccount(t *testing.T, h *harness, username string, n int) residueAccount {
	t.Helper()
	token := h.mustSignup(username, goodPass)
	var id int64
	var hash string
	if err := h.db.Reader.QueryRow("SELECT id, pw_hash FROM users WHERE username = ?", username).Scan(&id, &hash); err != nil {
		t.Fatal(err)
	}
	tx, err := h.db.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	for m := 1; m <= n; m++ {
		if _, err := tx.Exec("INSERT INTO ratings VALUES (?, ?, 4.5, ?)", id, m, residueTS); err != nil {
			t.Fatal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	if err := h.db.CheckpointTruncate(context.Background()); err != nil {
		t.Fatal(err)
	}
	a := residueAccount{token: token, username: username, hash: hash}
	if c := a.count(t, h); c == "" {
		t.Fatal("canary not found in app.db before the deletion; the probe is broken")
	}
	return a
}

// count describes every canary still present in app.db or app.db-wal, or
// returns "" when there are none.
func (a residueAccount) count(t *testing.T, h *harness) string {
	t.Helper()
	var out string
	for _, f := range []string{h.db.Path(), h.db.Path() + "-wal"} {
		b, err := os.ReadFile(f)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		for name, needle := range map[string][]byte{
			"rating rows": residueTSBytes,
			"username":    []byte(a.username),
			"pw_hash":     []byte(a.hash),
		} {
			if n := bytes.Count(b, needle); n > 0 {
				out += fmt.Sprintf(" %s: %d %s;", f[len(f)-6:], n, name)
			}
		}
	}
	return out
}

// eventually polls until the account leaves no bytes behind or the
// deadline passes.
func (a residueAccount) eventuallyGone(t *testing.T, h *harness, within time.Duration) {
	t.Helper()
	deadline := time.Now().Add(within)
	for {
		left := a.count(t, h)
		if left == "" {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("deleted account still on disk after %v:%s", within, left)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// Enough ratings to free whole pages: those go to the freelist, which
// secure_delete=FAST does not overwrite.
func TestDeletedAccountLeavesNoBytes(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	h.mustSignup("bystander", goodPass)
	a := newResidueAccount(t, h, "residueQuokka", 3000)
	if rec := h.do("DELETE", "/api/me", nil, a.token); rec.Code != 204 {
		t.Fatalf("delete: %d", rec.Code)
	}
	a.eventuallyGone(t, h, 5*time.Second)
}

// A reader holding a snapshot keeps any checkpoint from finishing. The
// deletion must not stall the writer meanwhile, and once the reader lets go
// the deleted bytes must still leave the WAL.
func TestDeletedAccountScrubbedAfterReaderReleases(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	a := newResidueAccount(t, h, "residueWombat", 3000)

	tx, err := h.db.Reader.Begin()
	if err != nil {
		t.Fatal(err)
	}
	var n int
	if err := tx.QueryRow("SELECT count(*) FROM ratings").Scan(&n); err != nil { // snapshot open
		t.Fatal(err)
	}

	start := time.Now()
	if rec := h.do("DELETE", "/api/me", nil, a.token); rec.Code != 204 {
		t.Fatalf("delete: %d", rec.Code)
	}
	if d := time.Since(start); d > time.Second {
		t.Errorf("DELETE /api/me took %v with a reader holding a snapshot", d)
	}
	// Writes keep flowing while the scrub waits for the reader.
	for i := 0; i < 5; i++ {
		start := time.Now()
		if _, err := h.db.Writer.Exec("INSERT INTO users (username, pw_hash, created_at) VALUES (?, 'x', 0)", fmt.Sprintf("writer%d", i)); err != nil {
			t.Fatal(err)
		}
		if d := time.Since(start); d > 500*time.Millisecond {
			t.Errorf("a write during the pending scrub took %v", d)
		}
		time.Sleep(100 * time.Millisecond)
	}

	_ = tx.Rollback()
	a.eventuallyGone(t, h, 10*time.Second)
}
