package feedback

import (
	"net/http"
	"strconv"
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
	// Another user's dismissals are their own.
	bob, bobTok := cachetest.NewUser(t, e.DB, "bob")
	if code := post(e, `{"movieId":1,"model":"pop","action":"not_interested"}`, bobTok); code != http.StatusNoContent {
		t.Fatal(code)
	}
	if got := notInterestedRows(t, e.DB, uid); len(got) != 1 || got[0] != 2959 {
		t.Errorf("alice rows %v", got)
	}
	if got := notInterestedRows(t, e.DB, bob); len(got) != 1 || got[0] != 1 {
		t.Errorf("bob rows %v", got)
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
	tx, err := e.DB.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < MaxNotInterested; i++ {
		// movie 100000 is the oldest (ts 1), then 100001 (ts 2), ...
		if _, err := tx.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, 'als', 'not_interested', ?)",
			uid, 100000+i, i+1); err != nil {
			t.Fatal(err)
		}
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
	if n := countFeedback(t, e.DB); n != MaxNotInterested+1 {
		t.Errorf("other actions must not be dropped: %d rows", n)
	}
}

func TestDeletedAccountMidRequest(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	h := &handler{db: e.DB, logger: nil, now: func() time.Time { return fixedNow }}
	if _, err := e.DB.Writer.Exec("DELETE FROM users WHERE id = ?", uid); err != nil {
		t.Fatal(err)
	}
	if err := h.notInterested(t.Context(), uid, 1, "als", 1); err != errNoUser {
		t.Errorf("err = %v, want errNoUser", err)
	}
	_ = tok
}
