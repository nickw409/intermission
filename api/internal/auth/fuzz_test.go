package auth

import (
	"errors"
	"testing"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// Go native fuzz targets for the auth JSON decoders. `make fuzz-smoke` runs
// each briefly; long runs are scheduled separately.

func checkDecodeErr(t *testing.T, err error) {
	t.Helper()
	var pub *apperr.Error
	if !errors.As(err, &pub) || pub.Status != 400 {
		t.Fatalf("decoder error is not a public 400: %v", err)
	}
}

func FuzzParseSignup(f *testing.F) {
	for _, seed := range []string{
		`{"username":"alice","password":"correct horse battery","invite_code":"ABCD-EFGH"}`,
		`{"username":"alice","password":"correct horse battery"}`,
		`{"username":"a b","password":"x"}`,
		`{"username":"alice","password":null}`,
		`{"username":"alice","password":"0123456789","extra":1}`,
		`{not json`, ``, `[]`, `null`, `"x"`, `{"username":"\u0000\u0000\u0000"}`,
		`{"username":"ééé","password":"éééééééééé"}`,
	} {
		f.Add([]byte(seed))
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		in, err := parseSignup(data)
		if err != nil {
			checkDecodeErr(t, err)
			return
		}
		if !validUsername(in.username) {
			t.Fatalf("accepted username %q", in.username)
		}
		if n := utf8.RuneCountInString(in.password); n < minPassword || n > maxPassword {
			t.Fatalf("accepted password of %d characters", n)
		}
		if utf8.RuneCountInString(in.inviteCode) > maxInviteCode {
			t.Fatalf("accepted invite code of %d characters", utf8.RuneCountInString(in.inviteCode))
		}
	})
}

func FuzzParseLogin(f *testing.F) {
	for _, seed := range []string{
		`{"username":"alice","password":"correct horse battery"}`,
		`{"username":"alice"}`,
		`{"username":1,"password":2}`,
		`{"username":"alice","password":"x"} trailing`,
		`{not json`, ``, `{}`, `{"USERNAME":"a","PASSWORD":"b"}`,
	} {
		f.Add([]byte(seed))
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		in, err := parseLogin(data)
		if err != nil {
			checkDecodeErr(t, err)
			return
		}
		if utf8.RuneCountInString(in.username) > maxLoginUsername || utf8.RuneCountInString(in.password) > maxPassword {
			t.Fatalf("accepted oversize login %d/%d", len(in.username), len(in.password))
		}
		_ = rateKey(in.username)
	})
}
