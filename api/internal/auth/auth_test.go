package auth

import (
	"crypto/sha256"
	"net/http"
	"net/http/httptest"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/invites"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// ---------------------------------------------------------------- signup

// Check order frozen in contracts/public.yaml: shape → invite (not consumed)
// → username → create + consume in one transaction.
func TestSignupCheckOrder(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	h.mustSignup("alice", goodPass)

	expired := h.mintInvite(5, time.Hour)
	usedUp := h.mintInvite(1, 0)
	if rec := h.do("POST", "/api/auth/signup", signupBody{"user1", goodPass, usedUp}, ""); rec.Code != 201 {
		t.Fatalf("using up invite: %d", rec.Code)
	}
	h.clock.Advance(2 * time.Hour)

	// 1. Shape errors win over a bad invite.
	rec := h.do("POST", "/api/auth/signup", signupBody{"a b", goodPass, "nonsense"}, "")
	if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeBadRequest {
		t.Fatalf("shape first: %d %s", rec.Code, rec.Body.String())
	}

	// 2. Without a valid invite, an existing username is never revealed.
	var bodies []string
	for name, code := range map[string]string{
		"unknown": "AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GG", "expired": expired, "used up": usedUp, "empty": "",
	} {
		for _, username := range []string{"alice", "ALICE", "newname"} {
			rec := h.do("POST", "/api/auth/signup", signupBody{username, goodPass, code}, "")
			if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeInvalidInvite {
				t.Errorf("%s invite, username %s: %d %s", name, username, rec.Code, rec.Body.String())
			}
			bodies = append(bodies, rec.Body.String())
		}
	}
	rec = h.do("POST", "/api/auth/signup", `{"username":"alice","password":"`+goodPass+`"}`, "")
	if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeInvalidInvite {
		t.Errorf("missing invite: %d %s", rec.Code, rec.Body.String())
	}
	bodies = append(bodies, rec.Body.String())
	for _, b := range bodies {
		if b != bodies[0] {
			t.Fatalf("invalid-invite bodies differ: %q vs %q", b, bodies[0])
		}
	}

	// 3. With a valid invite, a taken username is 409 and the invite is NOT consumed.
	valid := h.mintInvite(1, 0)
	rec = h.do("POST", "/api/auth/signup", signupBody{"Alice", goodPass, valid}, "")
	if rec.Code != 409 {
		t.Fatalf("taken: %d %s", rec.Code, rec.Body.String())
	}
	if e := errOf(t, rec); e.Code != apperr.CodeUsernameTaken || e.Message != "username is taken" {
		t.Fatalf("taken body: %+v", e)
	}
	if n := h.count("SELECT used_count FROM invites WHERE code_hash = ?", invites.Hash(valid)); n != 0 {
		t.Fatalf("409 consumed the invite: used_count = %d", n)
	}
	// 4. The same invite then works once for a free name.
	rec = h.do("POST", "/api/auth/signup", signupBody{"bob", goodPass, valid}, "")
	if rec.Code != 201 {
		t.Fatalf("signup bob: %d %s", rec.Code, rec.Body.String())
	}
	if m := meOf(t, rec); m.Username != "bob" || m.RatingCount != 0 || m.CreatedAt != h.clock.Now().Unix() {
		t.Fatalf("me = %+v", m)
	}
	if n := h.count("SELECT used_count FROM invites WHERE code_hash = ?", invites.Hash(valid)); n != 1 {
		t.Fatalf("used_count = %d", n)
	}
	rec = h.do("POST", "/api/auth/signup", signupBody{"carol", goodPass, valid}, "")
	if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeInvalidInvite {
		t.Fatalf("reuse past max_uses: %d", rec.Code)
	}
}

// Invalid, expired and used-up codes are indistinguishable (status and body).
func TestInvalidExpiredUsedUpInviteSameError(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	expired := h.mintInvite(1, time.Minute)
	used := h.mintInvite(1, 0)
	h.do("POST", "/api/auth/signup", signupBody{"first", goodPass, used}, "")
	h.clock.Advance(time.Minute)
	var got []string
	for _, code := range []string{"not-a-real-code", expired, used} {
		rec := h.do("POST", "/api/auth/signup", signupBody{"second", goodPass, code}, "")
		got = append(got, strconv.Itoa(rec.Code)+" "+rec.Body.String())
	}
	want := `400 {"error":{"code":"invalid_invite","message":"invite code is invalid"}}`
	for i, g := range got {
		if g != want {
			t.Errorf("case %d: %s", i, g)
		}
	}
}

func TestInviteCodeAcceptsTypingVariants(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	code := h.mintInvite(1, 0)
	typed := " " + strings.ToLower(strings.ReplaceAll(code, "-", "")) + " "
	if rec := h.do("POST", "/api/auth/signup", signupBody{"bob", goodPass, typed}, ""); rec.Code != 201 {
		t.Fatalf("lower-case, ungrouped code: %d %s", rec.Code, rec.Body.String())
	}
}

func TestOpenModeIgnoresInviteCode(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	for i, code := range []string{"", "garbage"} {
		rec := h.do("POST", "/api/auth/signup", signupBody{"user" + strconv.Itoa(i), goodPass, code}, "")
		if rec.Code != 201 {
			t.Fatalf("open signup: %d %s", rec.Code, rec.Body.String())
		}
	}
	if rec := h.do("POST", "/api/auth/signup", signupBody{"USER0", goodPass, ""}, ""); rec.Code != 409 {
		t.Fatalf("open mode taken: %d", rec.Code)
	}
	if n := h.count("SELECT count(*) FROM invites"); n != 0 {
		t.Fatalf("invites touched: %d", n)
	}
}

// 50 concurrent signups racing for an invite with max_uses = 5.
func TestConcurrentSignupsNeverExceedMaxUses(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	const maxUses = 5
	code := h.mintInvite(maxUses, 0)
	statuses := make([]int, 50)
	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := range statuses {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			rec := h.do("POST", "/api/auth/signup", signupBody{"racer" + strconv.Itoa(i), goodPass, code}, "")
			statuses[i] = rec.Code
			if rec.Code != 201 && (rec.Code != 400 || errOf(t, rec).Code != apperr.CodeInvalidInvite) {
				t.Errorf("racer %d: %d %s", i, rec.Code, rec.Body.String())
			}
		}()
	}
	close(start)
	wg.Wait()
	created := 0
	for _, s := range statuses {
		if s == 201 {
			created++
		}
	}
	if created != maxUses {
		t.Fatalf("%d signups succeeded, want %d", created, maxUses)
	}
	if n := h.count("SELECT used_count FROM invites"); n != maxUses {
		t.Fatalf("used_count = %d", n)
	}
	if n := h.count("SELECT count(*) FROM users"); n != maxUses {
		t.Fatalf("users = %d", n)
	}
	if n := h.count("SELECT count(*) FROM sessions"); n != maxUses {
		t.Fatalf("sessions = %d", n)
	}
}

func TestSignupFieldRules(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	ok := []signupBody{
		{"abc", goodPass, ""},
		{strings.Repeat("z", 32), goodPass, ""},
		{"A.b-c_9", goodPass, ""},
		{"tenrunes", strings.Repeat("é", 10), ""},        // 10 characters, 20 bytes
		{"maxlen", strings.Repeat("p", 128), ""},           // 128 characters
		{"maxrunes", strings.Repeat("€", 128), ""},         // 128 characters, 384 bytes
		{"inv64", goodPass, strings.Repeat("x", 64)},       // ignored in open mode, but within maxLength
		{"spaces", "  ten spaces and more  ", ""},          // passwords are not trimmed
	}
	for _, b := range ok {
		if rec := h.do("POST", "/api/auth/signup", b, ""); rec.Code != 201 {
			t.Errorf("%+v: %d %s", b, rec.Code, rec.Body.String())
		}
	}
	const userMsg = "username must be 3-32 characters of A-Z a-z 0-9 _ . -"
	const passMsg = "password must be 10-128 characters"
	bad := []struct {
		body any
		msg  string
	}{
		{signupBody{"ab", goodPass, ""}, userMsg},
		{signupBody{strings.Repeat("z", 33), goodPass, ""}, userMsg},
		{signupBody{"a b", goodPass, ""}, userMsg},
		{signupBody{"bad/name", goodPass, ""}, userMsg},
		{signupBody{"émile", goodPass, ""}, userMsg},
		{signupBody{"", goodPass, ""}, userMsg},
		{signupBody{"name\x00x", goodPass, ""}, userMsg},
		{signupBody{"shortpw", "123456789", ""}, passMsg},
		{signupBody{"shortrunes", strings.Repeat("é", 9), ""}, passMsg},
		{signupBody{"longpw", strings.Repeat("p", 129), ""}, passMsg},
		{signupBody{"inv65", goodPass, strings.Repeat("x", 65)}, "invite_code must be at most 64 characters"},
		{`{"password":"` + goodPass + `"}`, userMsg},
		{`{"username":"nopass"}`, passMsg},
		{`{"username":"nullpw","password":null}`, passMsg},
	}
	for _, c := range bad {
		rec := h.do("POST", "/api/auth/signup", c.body, "")
		if e := errOf(t, rec); rec.Code != 400 || e.Code != apperr.CodeBadRequest || e.Message != c.msg {
			t.Errorf("%v: %d %+v", c.body, rec.Code, e)
		}
	}
	if h.hasher.hashes.Load() != int64(len(ok)) {
		t.Errorf("hashes = %d: rejected signups must not hash", h.hasher.hashes.Load())
	}
}

// ---------------------------------------------------------------- login

func TestLoginSuccess(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	h.mustSignup("Alice", goodPass)
	for _, name := range []string{"Alice", "alice", "ALICE"} {
		rec := h.do("POST", "/api/auth/login", loginBody{name, goodPass}, "")
		if rec.Code != 200 {
			t.Fatalf("login %s: %d %s", name, rec.Code, rec.Body.String())
		}
		if m := meOf(t, rec); m.Username != "Alice" {
			t.Fatalf("me = %+v", m)
		}
		sessionToken(t, rec)
	}
}

// Unknown user and wrong password: same status, same body, same headers, and
// the same work (exactly one Argon2id verification, no other hashing).
func TestLoginErrorsIndistinguishable(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	h.mustSignup("alice", goodPass)
	type result struct {
		status  int
		body    string
		cookies int
		checks  int64
		hashes  int64
	}
	try := func(username, password string) result {
		h.hasher.reset()
		rec := h.do("POST", "/api/auth/login", loginBody{username, password}, "")
		return result{rec.Code, rec.Body.String(), len(rec.Header().Values("Set-Cookie")),
			h.hasher.checks.Load(), h.hasher.hashes.Load()}
	}
	wrongPass := try("alice", "wrong password!")
	unknown := try("nobody", "wrong password!")
	unknownSamePass := try("nobody", goodPass)
	want := result{401, `{"error":{"code":"invalid_credentials","message":"invalid username or password"}}`, 0, 1, 0}
	for name, r := range map[string]result{"wrong password": wrongPass, "unknown user": unknown, "unknown user, real password": unknownSamePass} {
		if r != want {
			t.Errorf("%s: %+v, want %+v", name, r, want)
		}
	}
}

// One coarse wall-clock check on top of the call-count check above: with
// realistic hashing cost, the two failure paths take about the same time.
func TestLoginTimingCoarse(t *testing.T) {
	if testing.Short() {
		t.Skip("timing")
	}
	params := Params{Time: 1, MemoryKiB: 16 * 1024, Threads: 4, SaltLen: 16, KeyLen: 32}
	h := newHarness(t, app.SignupOpen, func(o *options) { o.Hasher = NewArgon2id(params, MaxConcurrentHashes) })
	h.mustSignup("alice", goodPass)
	measure := func(username string) time.Duration {
		var ds []time.Duration
		for i := 0; i < 7; i++ {
			start := time.Now()
			if rec := h.do("POST", "/api/auth/login", loginBody{username, "wrong password!"}, ""); rec.Code != 401 {
				t.Fatalf("status %d", rec.Code)
			}
			ds = append(ds, time.Since(start))
		}
		sort.Slice(ds, func(i, j int) bool { return ds[i] < ds[j] })
		return ds[len(ds)/2]
	}
	known, unknown := measure("alice"), measure("nobody")
	ratio := float64(known) / float64(unknown)
	t.Logf("median wrong-password %v, unknown-user %v, ratio %.2f", known, unknown, ratio)
	if ratio < 0.5 || ratio > 2 {
		t.Fatalf("failure paths differ in time: wrong password %v vs unknown user %v", known, unknown)
	}
}

// The per-account limiter is keyed on the submitted, case-folded username,
// whether or not the account exists, so a 429 reveals nothing.
func TestLoginRateLimitPerAccount(t *testing.T) {
	h := newHarness(t, app.SignupOpen, func(o *options) { o.LoginPerAccount = ratelimit.LoginPerAccount })
	h.mustSignup("alice", goodPass)

	run := func(names []string, password string) []string {
		var out []string
		for _, n := range names {
			rec := h.do("POST", "/api/auth/login", loginBody{n, password}, "")
			out = append(out, strconv.Itoa(rec.Code)+" "+rec.Header().Get("Retry-After")+" "+rec.Body.String())
		}
		return out
	}
	// Mixed case all lands in one bucket.
	real := run([]string{"alice", "ALICE", "Alice", "aLiCe", "alicE", "alice", "ALICE"}, "wrong password!")
	ghost := run([]string{"ghost", "GHOST", "Ghost", "gHoSt", "ghosT", "ghost", "GHOST"}, "wrong password!")
	for i := range real {
		if real[i] != ghost[i] {
			t.Errorf("attempt %d differs: real %q vs unknown %q", i+1, real[i], ghost[i])
		}
	}
	for i := 0; i < 5; i++ {
		if !strings.HasPrefix(real[i], "401") {
			t.Fatalf("attempt %d: %s", i+1, real[i])
		}
	}
	for i := 5; i < 7; i++ {
		if !strings.HasPrefix(real[i], "429 12 ") || !strings.Contains(real[i], `"code":"rate_limited"`) {
			t.Fatalf("attempt %d: %s", i+1, real[i])
		}
	}
	// Limited attempts don't hash at all, and even the right password is refused.
	h.hasher.reset()
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""); rec.Code != 429 {
		t.Fatalf("correct password while limited: %d", rec.Code)
	}
	if h.hasher.checks.Load() != 0 {
		t.Fatal("limited attempt ran a hash")
	}
	// Other accounts are unaffected; after a minute alice may try again.
	if rec := h.do("POST", "/api/auth/login", loginBody{"bob", "x"}, ""); rec.Code != 401 {
		t.Fatalf("bob: %d", rec.Code)
	}
	h.clock.Advance(time.Minute)
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""); rec.Code != 200 {
		t.Fatalf("after a minute: %d", rec.Code)
	}
}

func TestGlobalCaps(t *testing.T) {
	h := newHarness(t, app.SignupOpen, func(o *options) {
		o.LoginGlobal = ratelimit.Config{Every: time.Minute, Burst: 3}
		o.SignupGlobal = ratelimit.Config{Every: time.Minute, Burst: 2}
	})
	for i := 0; i < 3; i++ {
		if rec := h.do("POST", "/api/auth/login", loginBody{"user" + strconv.Itoa(i), "whatever!!"}, ""); rec.Code != 401 {
			t.Fatalf("login %d: %d", i, rec.Code)
		}
	}
	rec := h.do("POST", "/api/auth/login", loginBody{"someone-else", "whatever!!"}, "")
	if rec.Code != 429 || rec.Header().Get("Retry-After") == "" || errOf(t, rec).Code != apperr.CodeRateLimited {
		t.Fatalf("login past global cap: %d %v", rec.Code, rec.Header())
	}
	for i := 0; i < 2; i++ {
		if rec := h.do("POST", "/api/auth/signup", signupBody{"new" + strconv.Itoa(i), goodPass, ""}, ""); rec.Code != 201 {
			t.Fatalf("signup %d: %d", i, rec.Code)
		}
	}
	rec = h.do("POST", "/api/auth/signup", signupBody{"new9", goodPass, ""}, "")
	if rec.Code != 429 || rec.Header().Get("Retry-After") != "60" {
		t.Fatalf("signup past global cap: %d %v", rec.Code, rec.Header())
	}
}

func TestLoginFieldRules(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	for _, body := range []any{
		`{"password":"x"}`,
		`{"username":"x"}`,
		loginBody{strings.Repeat("u", 65), goodPass},
		loginBody{"alice", strings.Repeat("p", 129)},
	} {
		rec := h.do("POST", "/api/auth/login", body, "")
		if rec.Code != 400 || errOf(t, rec).Code != apperr.CodeBadRequest {
			t.Errorf("%v: %d %s", body, rec.Code, rec.Body.String())
		}
	}
	if h.hasher.checks.Load() != 0 {
		t.Fatal("malformed login ran a hash")
	}
}

// ---------------------------------------------------------------- JSON and body limits

func TestAuthBodiesAndJSON(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	for _, path := range []string{"/api/auth/signup", "/api/auth/login"} {
		for _, c := range []struct {
			body   string
			status int
			code   apperr.Code
		}{
			{strings.Repeat("a", 65537), 413, apperr.CodeBodyTooLarge},
			{`{"username":"` + strings.Repeat("a", 65536) + `"}`, 413, apperr.CodeBodyTooLarge},
			{`{not json`, 400, apperr.CodeInvalidJSON},
			{``, 400, apperr.CodeInvalidJSON},
			{`{"username":"alice","password":"` + goodPass + `"}{}`, 400, apperr.CodeInvalidJSON},
			{`{"username":"alice","password":"` + goodPass + `","is_admin":true}`, 400, apperr.CodeBadRequest},
			{`{"username":["alice"],"password":"` + goodPass + `"}`, 400, apperr.CodeBadRequest},
			{`"alice"`, 400, apperr.CodeBadRequest},
		} {
			rec := h.do("POST", path, c.body, "")
			if rec.Code != c.status || errOf(t, rec).Code != c.code {
				t.Errorf("%s %.40q: %d %s", path, c.body, rec.Code, rec.Body.String())
			}
		}
	}
	if n := h.count("SELECT count(*) FROM users"); n != 0 {
		t.Fatalf("users created: %d", n)
	}
}

// ---------------------------------------------------------------- CSRF

func TestCSRFOnEveryMutatingAuthRoute(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	routes := []struct {
		method, path string
		body         any
	}{
		{"POST", "/api/auth/signup", signupBody{"mallory", goodPass, ""}},
		{"POST", "/api/auth/login", loginBody{"alice", goodPass}},
		{"POST", "/api/auth/logout", nil},
		{"DELETE", "/api/me", nil},
	}
	attacks := map[string]func(*http.Request){
		"no Origin":           func(r *http.Request) { r.Header.Del("Origin") },
		"foreign Origin":      func(r *http.Request) { r.Header.Set("Origin", "https://evil.example") },
		"no X-Requested-With": func(r *http.Request) { r.Header.Del("X-Requested-With") },
		"cross-site fetch":    func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "cross-site") },
	}
	for _, rt := range routes {
		for name, attack := range attacks {
			rec := h.do(rt.method, rt.path, rt.body, token, attack)
			if rec.Code != 403 || errOf(t, rec).Code != apperr.CodeCSRFRejected {
				t.Errorf("%s %s, %s: %d %s", rt.method, rt.path, name, rec.Code, rec.Body.String())
			}
			if len(rec.Header().Values("Set-Cookie")) != 0 {
				t.Errorf("%s %s, %s: Set-Cookie on a rejected request", rt.method, rt.path, name)
			}
		}
	}
	// Nothing happened: no new user, the session still works, alice exists.
	if n := h.count("SELECT count(*) FROM users"); n != 1 {
		t.Fatalf("users = %d", n)
	}
	if rec := h.do("GET", "/api/me", nil, token); rec.Code != 200 {
		t.Fatalf("session gone after rejected requests: %d", rec.Code)
	}
}

// ---------------------------------------------------------------- sessions

func TestSessionCookieAndStorage(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	rec := h.do("POST", "/api/auth/signup", signupBody{"alice", goodPass, ""}, "")
	c := setCookie(t, rec)
	if c.Name != "session" || !c.HttpOnly || !c.Secure || c.SameSite != http.SameSiteLaxMode || c.Path != "/" ||
		c.MaxAge != 30*24*3600 || c.Domain != "" {
		t.Fatalf("cookie = %+v (%s)", c, rec.Header().Get("Set-Cookie"))
	}
	raw := rec.Header().Get("Set-Cookie")
	for _, attr := range []string{"HttpOnly", "Secure", "SameSite=Lax", "Path=/", "Max-Age=2592000"} {
		if !strings.Contains(raw, attr) {
			t.Errorf("Set-Cookie %q lacks %s", raw, attr)
		}
	}
	// 32 random bytes, base64url without padding.
	if len(c.Value) != 43 || strings.ContainsAny(c.Value, "+/=") {
		t.Fatalf("token %q", c.Value)
	}
	if got := rec.Header().Get("Cache-Control"); !strings.Contains(got, "no-store") {
		t.Errorf("Cache-Control = %q", got)
	}
	// Only SHA-256(token) is stored.
	sum := sha256.Sum256([]byte(c.Value))
	if n := h.count("SELECT count(*) FROM sessions WHERE token_hash = ?", sum[:]); n != 1 {
		t.Fatal("sessions row is not SHA-256 of the token")
	}
	if n := h.count("SELECT count(*) FROM sessions WHERE instr(CAST(token_hash AS TEXT), ?) > 0 OR length(token_hash) != 32", c.Value); n != 0 {
		t.Fatal("raw token stored")
	}
	if n := h.count("SELECT count(*) FROM sessions WHERE expires_at - created_at = ?", 30*24*3600); n != 1 {
		t.Fatal("session lifetime is not 30 days")
	}
}

func TestExpiredAndLoggedOutSessionsRejected(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	if rec := h.do("GET", "/api/me", nil, token); rec.Code != 200 {
		t.Fatalf("fresh session: %d", rec.Code)
	}
	h.clock.Advance(30*24*time.Hour - time.Second)
	if rec := h.do("GET", "/api/me", nil, token); rec.Code != 200 {
		t.Fatalf("session one second before expiry: %d", rec.Code)
	}
	h.clock.Advance(time.Second)
	rec := h.do("GET", "/api/me", nil, token)
	if rec.Code != 401 || errOf(t, rec).Code != apperr.CodeUnauthorized || errOf(t, rec).Message != "not logged in" {
		t.Fatalf("expired session: %d %s", rec.Code, rec.Body.String())
	}

	// Logout deletes the session and clears the cookie.
	rec = h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, "")
	token = sessionToken(t, rec)
	rec = h.do("POST", "/api/auth/logout", nil, token)
	if rec.Code != 204 {
		t.Fatalf("logout: %d", rec.Code)
	}
	if c := setCookie(t, rec); c == nil || c.Value != "" || c.MaxAge != -1 || !c.HttpOnly || !c.Secure || c.Path != "/" {
		t.Fatalf("logout cookie = %+v", c)
	}
	for _, route := range [][2]string{{"GET", "/api/me"}, {"POST", "/api/auth/logout"}, {"DELETE", "/api/me"}} {
		if rec := h.do(route[0], route[1], nil, token); rec.Code != 401 {
			t.Fatalf("%v after logout: %d", route, rec.Code)
		}
	}
	if n := h.count("SELECT count(*) FROM users"); n != 1 {
		t.Fatal("DELETE /api/me with a dead session deleted the user")
	}
}

func TestSessionRotatesOnLogin(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	old := h.mustSignup("alice", goodPass)
	other := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, "") // a second device
	otherToken := sessionToken(t, other)

	rec := h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, old)
	if rec.Code != 200 {
		t.Fatalf("login: %d", rec.Code)
	}
	fresh := sessionToken(t, rec)
	if fresh == old {
		t.Fatal("login reused the presented session token")
	}
	if rec := h.do("GET", "/api/me", nil, old); rec.Code != 401 {
		t.Fatalf("old token still valid: %d", rec.Code)
	}
	if rec := h.do("GET", "/api/me", nil, fresh); rec.Code != 200 {
		t.Fatalf("new token: %d", rec.Code)
	}
	if rec := h.do("GET", "/api/me", nil, otherToken); rec.Code != 200 {
		t.Fatalf("another device's session was dropped: %d", rec.Code)
	}
	// A failed login does not end the presented session.
	if rec := h.do("POST", "/api/auth/login", loginBody{"alice", "wrong password!"}, fresh); rec.Code != 401 {
		t.Fatalf("bad login: %d", rec.Code)
	}
	if rec := h.do("GET", "/api/me", nil, fresh); rec.Code != 200 {
		t.Fatalf("session after failed login: %d", rec.Code)
	}
}

func TestGarbageCookiesRejected(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	for _, v := range []string{
		"x", "fixture-session-token", token[:42], token + "A", strings.Repeat("A", 43),
		strings.Repeat("A", 4096), "' OR '1'='1", token[:42] + "=",
	} {
		rec := h.do("GET", "/api/me", nil, v)
		if rec.Code != 401 || errOf(t, rec).Code != apperr.CodeUnauthorized {
			t.Errorf("cookie %.20q: %d", v, rec.Code)
		}
	}
	if rec := h.do("GET", "/api/me", nil, ""); rec.Code != 401 {
		t.Fatalf("no cookie: %d", rec.Code)
	}
}

// ---------------------------------------------------------------- account

func TestGetMeCountsRatings(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("Alice", goodPass)
	var id int64
	_ = h.db.Reader.QueryRow("SELECT id FROM users").Scan(&id)
	for m := 1; m <= 3; m++ {
		if _, err := h.db.Writer.Exec("INSERT INTO ratings VALUES (?, ?, 4.5, 0)", id, m); err != nil {
			t.Fatal(err)
		}
	}
	rec := h.do("GET", "/api/me", nil, token)
	if m := meOf(t, rec); rec.Code != 200 || m.Username != "Alice" || m.RatingCount != 3 || m.CreatedAt != 1794256680 {
		t.Fatalf("me: %d %+v", rec.Code, m)
	}
	if cc := rec.Header().Get("Cache-Control"); !strings.Contains(cc, "no-store") {
		t.Fatalf("Cache-Control %q", cc)
	}
}

func TestDeleteMeCascades(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	second := sessionToken(t, h.do("POST", "/api/auth/login", loginBody{"alice", goodPass}, ""))
	bobToken := h.mustSignup("bob", goodPass)
	ids := map[string]int64{}
	for _, n := range []string{"alice", "bob"} {
		var id int64
		_ = h.db.Reader.QueryRow("SELECT id FROM users WHERE username = ?", n).Scan(&id)
		ids[n] = id
		for m := 1; m <= 3; m++ {
			if _, err := h.db.Writer.Exec("INSERT INTO ratings VALUES (?, ?, 3.0, 0)", id, m); err != nil {
				t.Fatal(err)
			}
			if _, err := h.db.Writer.Exec("INSERT INTO feedback VALUES (?, ?, 'pop', 'click', 0)", id, m); err != nil {
				t.Fatal(err)
			}
		}
	}
	rec := h.do("DELETE", "/api/me", nil, token)
	if rec.Code != 204 || rec.Body.Len() != 0 {
		t.Fatalf("delete: %d %s", rec.Code, rec.Body.String())
	}
	if c := setCookie(t, rec); c == nil || c.MaxAge != -1 || c.Value != "" {
		t.Fatalf("cookie not cleared: %v", rec.Header().Values("Set-Cookie"))
	}
	a := ids["alice"]
	for table, q := range map[string]string{
		"users":    "SELECT count(*) FROM users WHERE id = ?",
		"sessions": "SELECT count(*) FROM sessions WHERE user_id = ?",
		"ratings":  "SELECT count(*) FROM ratings WHERE user_id = ?",
		"feedback": "SELECT count(*) FROM feedback WHERE user_id = ?",
	} {
		if n := h.count(q, a); n != 0 {
			t.Errorf("%s rows left for deleted user: %d", table, n)
		}
	}
	for _, tok := range []string{token, second} {
		if rec := h.do("GET", "/api/me", nil, tok); rec.Code != 401 {
			t.Errorf("deleted user's session still works: %d", rec.Code)
		}
	}
	// Bob is untouched.
	if n := h.count("SELECT count(*) FROM ratings WHERE user_id = ?", ids["bob"]); n != 3 {
		t.Fatalf("bob's ratings = %d", n)
	}
	if n := h.count("SELECT count(*) FROM feedback WHERE user_id = ?", ids["bob"]); n != 3 {
		t.Fatalf("bob's feedback = %d", n)
	}
	if rec := h.do("GET", "/api/me", nil, bobToken); rec.Code != 200 {
		t.Fatalf("bob: %d", rec.Code)
	}
	// The name is free again.
	if rec := h.do("POST", "/api/auth/signup", signupBody{"alice", goodPass, ""}, ""); rec.Code != 201 {
		t.Fatalf("re-signup: %d", rec.Code)
	}
}

// ---------------------------------------------------------------- logging

func TestLogsNeverContainSecrets(t *testing.T) {
	h := newHarness(t, app.SignupInvite)
	code := h.mintInvite(2, 0)
	const pw = "hunter2-very-secret-pw"
	rec := h.do("POST", "/api/auth/signup", signupBody{"alice", pw, code}, "")
	token := sessionToken(t, rec)
	h.do("POST", "/api/auth/signup", signupBody{"alice", pw, code}, "")
	h.do("POST", "/api/auth/login", loginBody{"alice", "wrong-secret-pw"}, "")
	tok2 := sessionToken(t, h.do("POST", "/api/auth/login", loginBody{"alice", pw}, token))
	h.do("GET", "/api/me", nil, tok2)
	h.do("POST", "/api/auth/logout", nil, tok2)
	logs := h.logs.String()
	for _, secret := range []string{pw, "wrong-secret-pw", token, tok2, code, strings.ReplaceAll(code, "-", "")} {
		if strings.Contains(logs, secret) {
			t.Fatalf("secret %q in logs:\n%s", secret, logs)
		}
	}
	if !strings.Contains(logs, `"user_id":`) {
		t.Fatalf("authenticated requests not attributed in logs:\n%s", logs)
	}
}

// ---------------------------------------------------------------- for other packages

func TestRequireSessionMiddleware(t *testing.T) {
	h := newHarness(t, app.SignupOpen)
	token := h.mustSignup("alice", goodPass)
	var gotID int64
	var gotKey string
	protected := RequireSession(&app.Deps{DB: h.db})(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotID, _ = UserID(r.Context())
		gotKey, _ = SessionKey(r)
		w.WriteHeader(http.StatusNoContent)
	}))
	req := httptest.NewRequest("GET", "/api/ratings", nil)
	req.AddCookie(&http.Cookie{Name: CookieName, Value: token})
	rec := httptest.NewRecorder()
	protected.ServeHTTP(rec, req)
	if rec.Code != 204 || gotID == 0 || len(gotKey) != 64 {
		t.Fatalf("status %d id %d key %q", rec.Code, gotID, gotKey)
	}
	rec = httptest.NewRecorder()
	protected.ServeHTTP(rec, httptest.NewRequest("GET", "/api/ratings", nil))
	if rec.Code != 401 || errOf(t, rec).Code != apperr.CodeUnauthorized {
		t.Fatalf("no session: %d", rec.Code)
	}
	if _, ok := SessionKey(httptest.NewRequest("GET", "/", nil)); ok {
		t.Fatal("SessionKey without a session")
	}
}
