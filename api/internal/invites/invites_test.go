package invites

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/base32"
	"errors"
	"io"
	"log/slog"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/db"
)

func newStore(t *testing.T) (*Store, *db.DB, *time.Time) {
	t.Helper()
	d, err := db.Open(context.Background(), filepath.Join(t.TempDir(), "app.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = d.Close() })
	if err := d.Migrate(context.Background(), slog.New(slog.NewTextHandler(io.Discard, nil))); err != nil {
		t.Fatal(err)
	}
	now := time.Unix(1_800_000_000, 0)
	s := New(d)
	s.now = func() time.Time { return now }
	return s, d, &now
}

var grouped = regexp.MustCompile(`^[A-Z2-7]{4}(-[A-Z2-7]{4}){5}-[A-Z2-7]{2}$`)

func TestGenerateFormat(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 200; i++ {
		code, err := Generate()
		if err != nil {
			t.Fatal(err)
		}
		if !grouped.MatchString(code) {
			t.Fatalf("code %q is not grouped base32", code)
		}
		raw, err := base32.StdEncoding.WithPadding(base32.NoPadding).DecodeString(strings.ReplaceAll(code, "-", ""))
		if err != nil || len(raw) != 16 {
			t.Fatalf("code %q decodes to %d bytes, %v", code, len(raw), err)
		}
		if seen[code] {
			t.Fatalf("duplicate code %q", code)
		}
		seen[code] = true
	}
}

func TestHashNormalizesTyping(t *testing.T) {
	want := Hash("ABCD-EFGH-IJKL")
	for _, typed := range []string{"abcd-efgh-ijkl", "ABCDEFGHIJKL", " abcd efgh ijkl ", "AbCd-EfGh-IjKl"} {
		if !bytes.Equal(Hash(typed), want) {
			t.Errorf("%q hashes differently", typed)
		}
	}
	if bytes.Equal(Hash("ABCD-EFGH-IJKM"), want) {
		t.Fatal("different codes hash equal")
	}
	if len(want) != 32 {
		t.Fatalf("hash length %d", len(want))
	}
}

func TestCreateStoresOnlyTheHash(t *testing.T) {
	s, d, _ := newStore(t)
	ctx := context.Background()
	code, err := s.Create(ctx, 3, 14*24*time.Hour, "for bob")
	if err != nil {
		t.Fatal(err)
	}
	var hash []byte
	var maxUses, used int
	var expires sql.NullInt64
	var note string
	err = d.Reader.QueryRow("SELECT code_hash, max_uses, used_count, expires_at, note FROM invites").Scan(&hash, &maxUses, &used, &expires, &note)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(hash, Hash(code)) || maxUses != 3 || used != 0 || note != "for bob" ||
		expires.Int64 != 1_800_000_000+14*24*3600 {
		t.Fatalf("row = %x %d %d %v %q", hash, maxUses, used, expires, note)
	}
	// The raw code appears nowhere in the database.
	var dump string
	if err := d.Reader.QueryRow("SELECT group_concat(quote(code_hash) || quote(note)) FROM invites").Scan(&dump); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(strings.ToUpper(dump), strings.ReplaceAll(code, "-", "")) {
		t.Fatal("raw code stored")
	}
	never, err := s.Create(ctx, 1, 0, "")
	if err != nil {
		t.Fatal(err)
	}
	if err := d.Reader.QueryRow("SELECT expires_at FROM invites WHERE code_hash = ?", Hash(never)).Scan(&expires); err != nil || expires.Valid {
		t.Fatalf("no-expiry invite: %v %v", expires, err)
	}
	for _, bad := range []struct {
		uses int
		exp  time.Duration
	}{{0, time.Hour}, {-1, time.Hour}, {1, -time.Hour}, {MaxUses + 1, time.Hour}} {
		if _, err := s.Create(ctx, bad.uses, bad.exp, ""); err == nil {
			t.Errorf("Create(%d, %v) accepted", bad.uses, bad.exp)
		}
	}
}

func TestInvalidExpiredAndUsedUpAreTheSameError(t *testing.T) {
	s, _, now := newStore(t)
	ctx := context.Background()
	expired, _ := s.Create(ctx, 1, time.Hour, "")
	used, _ := s.Create(ctx, 1, 0, "")
	if err := s.redeemOnce(ctx, used); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(2 * time.Hour)
	for name, code := range map[string]string{"unknown": "AAAA-BBBB", "empty": "", "expired": expired, "used up": used} {
		if err := s.Check(ctx, code); !errors.Is(err, ErrInvalid) {
			t.Errorf("Check %s: %v", name, err)
		}
		if err := s.redeemOnce(ctx, code); !errors.Is(err, ErrInvalid) {
			t.Errorf("Redeem %s: %v", name, err)
		}
	}
	// Expiry is exclusive: an invite is dead at its expires_at second.
	edge, _ := s.Create(ctx, 1, time.Hour, "")
	*now = now.Add(time.Hour)
	if err := s.Check(ctx, edge); !errors.Is(err, ErrInvalid) {
		t.Fatalf("invite valid at its expiry instant: %v", err)
	}
}

func TestCheckDoesNotConsume(t *testing.T) {
	s, d, _ := newStore(t)
	ctx := context.Background()
	code, _ := s.Create(ctx, 1, 0, "")
	for i := 0; i < 5; i++ {
		if err := s.Check(ctx, code); err != nil {
			t.Fatal(err)
		}
	}
	var used int
	_ = d.Reader.QueryRow("SELECT used_count FROM invites").Scan(&used)
	if used != 0 {
		t.Fatalf("Check consumed: used_count = %d", used)
	}
	if err := s.redeemOnce(ctx, code); err != nil {
		t.Fatal(err)
	}
	if err := s.redeemOnce(ctx, code); !errors.Is(err, ErrInvalid) {
		t.Fatalf("second redeem: %v", err)
	}
}

func TestConcurrentRedeemNeverExceedsMaxUses(t *testing.T) {
	s, d, _ := newStore(t)
	ctx := context.Background()
	const maxUses = 5
	code, _ := s.Create(ctx, maxUses, 0, "")
	var ok atomic.Int32
	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			err := s.redeemOnce(ctx, code)
			switch {
			case err == nil:
				ok.Add(1)
			case !errors.Is(err, ErrInvalid):
				t.Errorf("redeem: %v", err)
			}
		}()
	}
	close(start)
	wg.Wait()
	var used int
	_ = d.Reader.QueryRow("SELECT used_count FROM invites").Scan(&used)
	if ok.Load() != maxUses || used != maxUses {
		t.Fatalf("succeeded %d, used_count %d, want %d", ok.Load(), used, maxUses)
	}
}

// redeemOnce runs Redeem in its own write transaction.
func (s *Store) redeemOnce(ctx context.Context, code string) error {
	tx, err := s.db.Writer.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	if err := s.Redeem(ctx, tx, code); err != nil {
		return err
	}
	return tx.Commit()
}
