package invites

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base32"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/nickw409/intermission/api/internal/db"
)

// CodeBytes is the amount of randomness in a code (128 bits).
const CodeBytes = 16

// MaxUses bounds --uses for one code.
const MaxUses = 1000

// ErrInvalid is the single error for unknown, expired and used-up codes, so a
// caller learns nothing about which one it was.
var ErrInvalid = errors.New("invite code is invalid")

var encoding = base32.StdEncoding.WithPadding(base32.NoPadding)

// Generate returns a fresh code: 16 bytes from crypto/rand, base32-encoded
// (26 characters, A-Z and 2-7) and grouped in fours for reading aloud, e.g.
// ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YZ.
func Generate() (string, error) {
	var raw [CodeBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	s := encoding.EncodeToString(raw[:])
	var b strings.Builder
	for i := 0; i < len(s); i += 4 {
		if i > 0 {
			b.WriteByte('-')
		}
		b.WriteString(s[i:min(i+4, len(s))])
	}
	return b.String(), nil
}

// normalize undoes harmless typing differences: case, dashes and spaces.
func normalize(code string) string {
	return strings.ToUpper(strings.NewReplacer("-", "", " ", "").Replace(code))
}

// Hash is what the database stores: SHA-256 of the normalized code. A plain
// hash (no salt or slow KDF) is right here because the code itself carries
// 128 random bits; nothing about it can be guessed.
func Hash(code string) []byte {
	sum := sha256.Sum256([]byte(normalize(code)))
	return sum[:]
}

// Store reads and writes the invites table.
type Store struct {
	db  *db.DB
	now func() time.Time
}

// New returns a Store over d.
func New(d *db.DB) *Store { return &Store{db: d, now: time.Now} }

// Create mints a code usable uses times, expiring after expiresIn (0 means
// never), and returns it. Only its hash is stored, so this is the one time
// the code can be shown.
func (s *Store) Create(ctx context.Context, uses int, expiresIn time.Duration, note string) (string, error) {
	if uses < 1 || uses > MaxUses {
		return "", fmt.Errorf("uses must be between 1 and %d", MaxUses)
	}
	if expiresIn < 0 {
		return "", errors.New("expiry must not be negative")
	}
	code, err := Generate()
	if err != nil {
		return "", err
	}
	now := s.now()
	var expires sql.NullInt64
	if expiresIn > 0 {
		expires = sql.NullInt64{Int64: now.Add(expiresIn).Unix(), Valid: true}
	}
	var noteVal sql.NullString
	if note != "" {
		noteVal = sql.NullString{String: note, Valid: true}
	}
	_, err = s.db.Writer.ExecContext(ctx,
		"INSERT INTO invites (code_hash, max_uses, used_count, expires_at, note, created_at) VALUES (?, ?, 0, ?, ?, ?)",
		Hash(code), uses, expires, noteVal, now.Unix())
	if err != nil {
		return "", err
	}
	return code, nil
}

// Check reports whether code is currently redeemable, without consuming it.
// It returns ErrInvalid for unknown, expired and used-up codes alike.
func (s *Store) Check(ctx context.Context, code string) error {
	if normalize(code) == "" {
		return ErrInvalid
	}
	var one int
	err := s.db.Reader.QueryRowContext(ctx,
		"SELECT 1 FROM invites WHERE code_hash = ? AND used_count < max_uses AND (expires_at IS NULL OR expires_at > ?)",
		Hash(code), s.now().Unix()).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrInvalid
	}
	return err
}

// Redeem consumes one use of code inside tx, the transaction that creates the
// user. The check and the increment are a single conditional UPDATE, and
// writer transactions are serialized (one writer connection, BEGIN
// IMMEDIATE), so a code can never be redeemed more than max_uses times.
func (s *Store) Redeem(ctx context.Context, tx *sql.Tx, code string) error {
	if normalize(code) == "" {
		return ErrInvalid
	}
	res, err := tx.ExecContext(ctx,
		"UPDATE invites SET used_count = used_count + 1 WHERE code_hash = ? AND used_count < max_uses AND (expires_at IS NULL OR expires_at > ?)",
		Hash(code), s.now().Unix())
	if err != nil {
		return err
	}
	n, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if n != 1 {
		return ErrInvalid
	}
	return nil
}
