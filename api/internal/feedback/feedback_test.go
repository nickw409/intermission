package feedback

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"slices"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

var fixedNow = time.Unix(1794259200, 0)

func newEnv(t *testing.T) *cachetest.Env {
	t.Helper()
	return cachetest.NewEnv(t, nil, func(mux *http.ServeMux, d *app.Deps) {
		register(mux, d, func() time.Time { return fixedNow })
	})
}

func post(e *cachetest.Env, body, token string) int {
	return e.Do("POST", "/api/feedback", body, token).Code
}

func notInterestedRows(t *testing.T, d *db.DB, uid int64) []int64 {
	t.Helper()
	rows, err := d.Reader.Query("SELECT movie_id FROM feedback WHERE user_id = ? AND action = 'not_interested' ORDER BY movie_id", uid)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			t.Fatal(err)
		}
		out = append(out, id)
	}
	return out
}

func countFeedback(t *testing.T, d *db.DB) int {
	t.Helper()
	var n int
	if err := d.Reader.QueryRow("SELECT count(*) FROM feedback").Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

// ---------------------------------------------------------------- contract

func TestContractFixtures(t *testing.T) {
	for _, c := range cachetest.PublicCases(t, "postFeedback") {
		t.Run(c.Name, func(t *testing.T) {
			e := newEnv(t)
			uid, tok := cachetest.NewUser(t, e.DB, "alice")
			if c.Name == "rate_limited" {
				// The production limit (60 per minute per session), exhausted.
				for i := 0; i < 60; i++ {
					if code := post(e, `{"movieId":2959,"model":"als","action":"click"}`, tok); code != http.StatusNoContent {
						t.Fatalf("request %d: %d", i, code)
					}
				}
				// Retry-After depends on the bucket shape; check it by shape below.
				delete(c.Response.Headers, "Retry-After")
			}
			before := cachetest.RatingsVersion(t, e.DB, uid)
			rec := e.Replay(c, tok)
			if c.Name == "rate_limited" {
				if n, err := strconv.Atoi(rec.Header().Get("Retry-After")); err != nil || n < 1 || n > 60 {
					t.Errorf("Retry-After %q", rec.Header().Get("Retry-After"))
				}
			}
			after := cachetest.RatingsVersion(t, e.DB, uid)
			stored := notInterestedRows(t, e.DB, uid)
			if c.Name == "ok" {
				if after != before+1 || len(stored) != 1 || stored[0] != 2959 {
					t.Errorf("ok: version %d -> %d, stored %v", before, after, stored)
				}
			} else if after != before || len(stored) != 0 {
				t.Errorf("%s: version %d -> %d, stored %v", c.Name, before, after, stored)
			}
		})
	}
}

// ---------------------------------------------------------------- behaviour

func TestNotInterestedIsStoredOnceAndBumpsVersion(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	v0 := cachetest.RatingsVersion(t, e.DB, uid)
	if code := post(e, `{"movieId":2959,"model":"knn","action":"not_interested"}`, tok); code != http.StatusNoContent {
		t.Fatalf("first: %d", code)
	}
	v1 := cachetest.RatingsVersion(t, e.DB, uid)
	if v1 != v0+1 {
		t.Errorf("version %d -> %d, want +1", v0, v1)
	}
	var model string
	var ts int64
	if err := e.DB.Reader.QueryRow("SELECT model, ts FROM feedback WHERE user_id = ?", uid).Scan(&model, &ts); err != nil {
		t.Fatal(err)
	}
	if model != "knn" || ts != fixedNow.Unix() {
		t.Errorf("row model %q ts %d", model, ts)
	}
	// The same movie again (from any model) changes nothing.
	if code := post(e, `{"movieId":2959,"model":"als","action":"not_interested"}`, tok); code != http.StatusNoContent {
		t.Fatalf("repeat: %d", code)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != v1 {
		t.Errorf("repeat bumped version %d -> %d", v1, v)
	}
	if got := notInterestedRows(t, e.DB, uid); len(got) != 1 {
		t.Errorf("rows %v", got)
	}
	// Another user's dismissals are their own, even for the same movie.
	bob, bobTok := cachetest.NewUser(t, e.DB, "bob")
	bob0 := cachetest.RatingsVersion(t, e.DB, bob)
	if code := post(e, `{"movieId":2959,"model":"pop","action":"not_interested"}`, bobTok); code != http.StatusNoContent {
		t.Fatal(code)
	}
	if got := notInterestedRows(t, e.DB, uid); len(got) != 1 || got[0] != 2959 {
		t.Errorf("alice rows %v", got)
	}
	if got := notInterestedRows(t, e.DB, bob); len(got) != 1 || got[0] != 2959 {
		t.Errorf("bob rows %v", got)
	}
	if v := cachetest.RatingsVersion(t, e.DB, bob); v != bob0+1 {
		t.Errorf("bob version %d -> %d", bob0, v)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != v1 {
		t.Errorf("bob's write bumped alice's version %d -> %d", v1, v)
	}
}

func TestImpressionAndClickAreNotStoredYet(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	v0 := cachetest.RatingsVersion(t, e.DB, uid)
	for _, action := range []string{"impression", "click"} {
		if code := post(e, `{"movieId":2959,"model":"als","action":"`+action+`"}`, tok); code != http.StatusNoContent {
			t.Errorf("%s: %d", action, code)
		}
	}
	if n := countFeedback(t, e.DB); n != 0 {
		t.Errorf("%d feedback rows", n)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != v0 {
		t.Errorf("version %d -> %d", v0, v)
	}
}

func TestValidation(t *testing.T) {
	e := newEnv(t)
	_, tok := cachetest.NewUser(t, e.DB, "alice")
	for body, code := range map[string]string{
		`{"movieId":2959,"model":"als","action":"love"}`:                     "bad_request",
		`{"movieId":2959,"model":"als"}`:                                     "bad_request",
		`{"movieId":2959,"model":"als","action":null}`:                       "bad_request",
		`{"movieId":0,"model":"als","action":"not_interested"}`:              "bad_request",
		`{"movieId":-5,"model":"als","action":"not_interested"}`:             "bad_request",
		`{"movieId":2147483648,"model":"als","action":"not_interested"}`:     "bad_request",
		`{"movieId":2959.5,"model":"als","action":"not_interested"}`:         "bad_request",
		`{"movieId":"2959","model":"als","action":"not_interested"}`:         "bad_request",
		`{"model":"als","action":"not_interested"}`:                          "bad_request",
		`{"movieId":2959,"model":"svd","action":"not_interested"}`:           "bad_request",
		`{"movieId":2959,"action":"not_interested"}`:                         "bad_request",
		`{"movieId":2959,"model":"als","action":"not_interested","extra":1}`: "bad_request",
		`{"movieId":2959,"model":"als","action":"Not_Interested"}`:           "bad_request",
		`[]`: "bad_request",
		``:   "invalid_json",
		`{"movieId":2959,"model":"als","action":"not_interested"}{}`: "invalid_json",
	} {
		rec := e.Do("POST", "/api/feedback", body, tok)
		if rec.Code != http.StatusBadRequest || cachetest.ErrorCode(rec) != code {
			t.Errorf("%s: %d %s, want 400 %s", body, rec.Code, rec.Body.String(), code)
		}
	}
	if n := countFeedback(t, e.DB); n != 0 {
		t.Errorf("%d rows after invalid requests", n)
	}
}

// At the cap the oldest dismissal makes room, so the newest one always counts.
func TestNotInterestedCapDropsOldest(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	bob, _ := cachetest.NewUser(t, e.DB, "bob") // before the transaction: the writer has one connection
	tx, err := e.DB.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	// Inserted newest first, so rowid order is the reverse of ts order: the
	// cap must go by ts. movie 100000 is the oldest (ts 1), then 100001, ...
	for i := MaxNotInterested - 1; i >= 0; i-- {
		if _, err := tx.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, 'als', 'not_interested', ?)",
			uid, 100000+i, i+1); err != nil {
			t.Fatal(err)
		}
	}
	// Another user's even older dismissal is not touched.
	if _, err := tx.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 9, 'als', 'not_interested', 0)", bob); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 7, 'als', 'click', 0)", uid); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	if code := post(e, `{"movieId":5,"model":"als","action":"not_interested"}`, tok); code != http.StatusNoContent {
		t.Fatal(code)
	}
	got := notInterestedRows(t, e.DB, uid)
	if len(got) != MaxNotInterested {
		t.Fatalf("%d rows, want %d", len(got), MaxNotInterested)
	}
	if got[0] != 5 || got[1] != 100001 {
		t.Errorf("first rows %v: want the new movie 5 and the oldest (100000) gone", got[:2])
	}
	if n := countFeedback(t, e.DB); n != MaxNotInterested+2 {
		t.Errorf("other actions and users must not be dropped: %d rows", n)
	}
	if got := notInterestedRows(t, e.DB, bob); len(got) != 1 || got[0] != 9 {
		t.Errorf("bob rows %v", got)
	}
}

// The account is deleted between the session check and the write: the write
// rolls back and answers 401 rather than failing on the foreign key.
func TestDeletedAccountBeforeWrite(t *testing.T) {
	e := newEnv(t)
	uid, _ := cachetest.NewUser(t, e.DB, "alice")
	h := &handler{db: e.DB, now: func() time.Time { return fixedNow }}
	if _, err := e.DB.Writer.Exec("DELETE FROM users WHERE id = ?", uid); err != nil {
		t.Fatal(err)
	}
	if err := h.notInterested(t.Context(), uid, 1, "als", 1); !errors.Is(err, errNoUser) {
		t.Errorf("err = %v, want errNoUser", err)
	}
	if n := countFeedback(t, e.DB); n != 0 {
		t.Errorf("%d rows", n)
	}
}

// Concurrent requests for the same movie store it once and bump once.
func TestConcurrentSameMovie(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	v0 := cachetest.RatingsVersion(t, e.DB, uid)
	var wg sync.WaitGroup
	codes := make([]int, 20)
	for i := range codes {
		wg.Add(1)
		go func() {
			defer wg.Done()
			codes[i] = post(e, `{"movieId":2959,"model":"als","action":"not_interested"}`, tok)
		}()
	}
	wg.Wait()
	for i, c := range codes {
		if c != http.StatusNoContent {
			t.Errorf("request %d: %d", i, c)
		}
	}
	if got := notInterestedRows(t, e.DB, uid); len(got) != 1 {
		t.Errorf("rows %v", got)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != v0+1 {
		t.Errorf("version %d -> %d, want +1", v0, v)
	}
}

// The database itself refuses a second "not interested" row for the same
// user and movie (migration 0004), whatever the handler does.
func TestUniqueIndex(t *testing.T) {
	e := newEnv(t)
	uid, _ := cachetest.NewUser(t, e.DB, "alice")
	ins := "INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 5, 'als', ?, 1)"
	if _, err := e.DB.Writer.Exec(ins, uid, "not_interested"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.DB.Writer.Exec(ins, uid, "not_interested"); err == nil {
		t.Error("duplicate not_interested row accepted")
	}
	for i := 0; i < 2; i++ { // other actions may repeat
		if _, err := e.DB.Writer.Exec(ins, uid, "click"); err != nil {
			t.Fatal(err)
		}
	}
}

// More sessions on one account do not add up to more writes: the per-user
// bucket (same shape as the per-session one) runs out too.
func TestPerUserLimitAcrossSessions(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	second := base64.RawURLEncoding.EncodeToString(sha256.New().Sum([]byte("second"))[:32]) // 32 bytes, like a real token
	sum := sha256.Sum256([]byte(second))
	now := time.Now().Unix()
	if _, err := e.DB.Writer.Exec("INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
		sum[:], uid, now, now+3600); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 60; i++ {
		if code := post(e, `{"movieId":2959,"model":"als","action":"click"}`, tok); code != http.StatusNoContent {
			t.Fatalf("request %d: %d", i, code)
		}
	}
	if code := post(e, `{"movieId":2959,"model":"als","action":"click"}`, second); code != http.StatusTooManyRequests {
		t.Errorf("second session got %d, want 429 from the per-user bucket", code)
	}
	// Another user is unaffected.
	_, bobTok := cachetest.NewUser(t, e.DB, "bob")
	if code := post(e, `{"movieId":2959,"model":"als","action":"click"}`, bobTok); code != http.StatusNoContent {
		t.Errorf("bob got %d", code)
	}
}

// ---------------------------------------------------------------- list and undo

func seedNotInterested(t *testing.T, d *db.DB, uid, movie, ts int64) {
	t.Helper()
	if _, err := d.Writer.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, 'als', 'not_interested', ?)", uid, movie, ts); err != nil {
		t.Fatal(err)
	}
}

func TestListAndUndoContractFixtures(t *testing.T) {
	for _, op := range []string{"listNotInterested", "deleteNotInterested"} {
		for _, c := range cachetest.PublicCases(t, op) {
			t.Run(op+"/"+c.Name, func(t *testing.T) {
				e := newEnv(t)
				uid, tok := cachetest.NewUser(t, e.DB, "alice")
				cachetest.SeedMovies(t, e.DB, cachetest.Movie(2959, "Fight Club", 1999), cachetest.Movie(1, "Toy Story", 1995))
				seedNotInterested(t, e.DB, uid, 1, 1794258000)
				seedNotInterested(t, e.DB, uid, 2959, 1794259200)
				before := cachetest.RatingsVersion(t, e.DB, uid)
				e.Replay(c, tok)
				after := cachetest.RatingsVersion(t, e.DB, uid)
				undone := op == "deleteNotInterested" && c.Name == "ok"
				if (after == before+1) != undone || after < before {
					t.Errorf("ratings_version %d -> %d", before, after)
				}
				want := []int64{1, 2959}
				if undone {
					want = []int64{1}
				}
				if got := notInterestedRows(t, e.DB, uid); !slices.Equal(got, want) {
					t.Errorf("rows %v, want %v", got, want)
				}
			})
		}
	}
}

func TestUndo(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	bob, _ := cachetest.NewUser(t, e.DB, "bob")
	seedNotInterested(t, e.DB, uid, 5, 1)
	seedNotInterested(t, e.DB, bob, 5, 1)
	if _, err := e.DB.Writer.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 5, 'als', 'click', 1)", uid); err != nil {
		t.Fatal(err)
	}
	v0 := cachetest.RatingsVersion(t, e.DB, uid)
	if rec := e.Do("DELETE", "/api/feedback/not-interested/5", nil, tok); rec.Code != http.StatusNoContent || rec.Body.Len() != 0 {
		t.Fatalf("%d %q", rec.Code, rec.Body.String())
	}
	if got := notInterestedRows(t, e.DB, uid); len(got) != 0 {
		t.Errorf("alice rows %v", got)
	}
	if got := notInterestedRows(t, e.DB, bob); len(got) != 1 {
		t.Errorf("bob's row went too: %v", got)
	}
	if n := countFeedback(t, e.DB); n != 2 { // bob's not_interested and alice's click
		t.Errorf("%d rows left", n)
	}
	v1 := cachetest.RatingsVersion(t, e.DB, uid)
	if v1 != v0+1 {
		t.Errorf("version %d -> %d, want +1", v0, v1)
	}
	// Again: nothing to undo, still 204, no bump.
	if rec := e.Do("DELETE", "/api/feedback/not-interested/5", nil, tok); rec.Code != http.StatusNoContent {
		t.Fatalf("repeat: %d", rec.Code)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != v1 {
		t.Errorf("repeat bumped %d -> %d", v1, v)
	}
	// Marking it again after an undo works.
	if code := post(e, `{"movieId":5,"model":"knn","action":"not_interested"}`, tok); code != http.StatusNoContent {
		t.Fatal(code)
	}
	if got := notInterestedRows(t, e.DB, uid); !slices.Equal(got, []int64{5}) {
		t.Errorf("rows %v", got)
	}
	for _, bad := range []string{"0", "-1", "abc", "99999999999"} {
		if rec := e.Do("DELETE", "/api/feedback/not-interested/"+bad, nil, tok); rec.Code != http.StatusBadRequest {
			t.Errorf("%s: %d", bad, rec.Code)
		}
	}
}

func TestListNewestFirstWithPlaceholderTitles(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	bob, _ := cachetest.NewUser(t, e.DB, "bob")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000))
	seedNotInterested(t, e.DB, uid, 10, 5)
	seedNotInterested(t, e.DB, uid, 424242, 9) // not in the cache and unknown to recs-model
	seedNotInterested(t, e.DB, bob, 11, 7)
	if _, err := e.DB.Writer.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 12, 'als', 'click', 8)", uid); err != nil {
		t.Fatal(err)
	}
	rec := e.Do("GET", "/api/feedback/not-interested", nil, tok)
	if rec.Code != http.StatusOK {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	var got NotInterestedList
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Items) != 2 || got.Items[0].MovieID != 424242 || got.Items[0].Title != "Movie 424242" || got.Items[0].Year != nil ||
		got.Items[1].MovieID != 10 || got.Items[1].Title != "Ten" || got.Items[1].HiddenAt != 5 {
		t.Errorf("items %+v", got.Items)
	}
	// An empty list is [], not null.
	_, carolTok := cachetest.NewUser(t, e.DB, "carol")
	if rec := e.Do("GET", "/api/feedback/not-interested", nil, carolTok); rec.Code != 200 || rec.Body.String() != `{"items":[]}` {
		t.Errorf("empty: %d %s", rec.Code, rec.Body.String())
	}
}
