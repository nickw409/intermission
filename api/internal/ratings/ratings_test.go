package ratings

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/auth"
	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
	"github.com/nickw409/intermission/api/internal/ratelimit"
)

// fixedNow is the updated_at in contracts/fixtures/public/putRating.json.
var fixedNow = time.Unix(1794259200, 0)

func newEnv(t *testing.T) *cachetest.Env {
	t.Helper()
	return cachetest.NewEnv(t, nil, func(mux *http.ServeMux, d *app.Deps) {
		register(mux, d, func() time.Time { return fixedNow })
	})
}

type ratingRow struct {
	movie   int64
	rating  float64
	updated int64
}

func seedRatings(t *testing.T, d *db.DB, uid int64, rows ...ratingRow) {
	t.Helper()
	tx, err := d.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback() }()
	for _, r := range rows {
		if _, err := tx.Exec("INSERT INTO ratings (user_id, movie_id, rating, updated_at) VALUES (?, ?, ?, ?)", uid, r.movie, r.rating, r.updated); err != nil {
			t.Fatal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
}

// aliceRatings are the ratings behind listRatings.json's "ok" case.
var aliceRatings = []ratingRow{
	{260, 5, 1794256872}, {2571, 4.5, 1794256855}, {296, 4.5, 1794256831}, {1, 4, 1794256802}, {318, 4, 1794256787},
}

func manyRatings(n int, firstMovie int64) []ratingRow {
	rows := make([]ratingRow, n)
	for i := range rows {
		rows[i] = ratingRow{firstMovie + int64(i), 3, 1}
	}
	return rows
}

func countRatings(t *testing.T, d *db.DB, uid int64) int {
	t.Helper()
	var n int
	if err := d.Reader.QueryRow("SELECT count(*) FROM ratings WHERE user_id = ?", uid).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

// ---------------------------------------------------------------- contract

func TestContractFixtures(t *testing.T) {
	for _, op := range []string{"listRatings", "putRating", "deleteRating"} {
		for _, c := range cachetest.PublicCases(t, op) {
			t.Run(op+"/"+c.Name, func(t *testing.T) {
				e := newEnv(t)
				uid, tok := cachetest.NewUser(t, e.DB, "alice")
				switch {
				case op == "listRatings":
					seedRatings(t, e.DB, uid, aliceRatings...)
				case c.Name == "too_many_ratings":
					seedRatings(t, e.DB, uid, manyRatings(MaxRatingsPerUser, 100000)...)
				case op == "deleteRating" && c.Name == "ok":
					seedRatings(t, e.DB, uid, ratingRow{4993, 4.5, 1})
				}
				if c.Name == "model_unavailable" {
					e.Fake.SetDown(true)
				}
				if c.Name == "rate_limited" {
					// The production limit (60 per minute per user), exhausted
					// by no-op deletes.
					for i := 0; i < 60; i++ {
						if rec := e.Do("DELETE", "/api/ratings/1", nil, tok); rec.Code != http.StatusNoContent {
							t.Fatalf("delete %d: %d", i, rec.Code)
						}
					}
				}
				before := cachetest.RatingsVersion(t, e.DB, uid)
				rec := e.Replay(c, tok)
				after := cachetest.RatingsVersion(t, e.DB, uid)
				wantBump := c.Response.Status/100 == 2 && op != "listRatings"
				if (after == before+1) != wantBump || after < before {
					t.Errorf("ratings_version %d -> %d (status %d)", before, after, rec.Code)
				}
			})
		}
	}
}

// ---------------------------------------------------------------- validation

func TestRatingValidation(t *testing.T) {
	e := newEnv(t)
	_, tok := cachetest.NewUser(t, e.DB, "alice")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000))
	for _, ok := range []string{`{"rating":0.5}`, `{"rating":5}`, `{"rating":5.0}`, `{"rating":3}`, `{"rating":2.5}`, `{"rating":45e-1}`} {
		if rec := e.Do("PUT", "/api/ratings/10", ok, tok); rec.Code != http.StatusOK {
			t.Errorf("%s: %d %s", ok, rec.Code, rec.Body.String())
		}
	}
	for body, code := range map[string]string{
		`{"rating":0}`:              "bad_request",
		`{"rating":5.5}`:            "bad_request",
		`{"rating":4.3}`:            "bad_request",
		`{"rating":4.25}`:           "bad_request",
		`{"rating":-1}`:             "bad_request",
		`{"rating":1e308}`:          "bad_request",
		`{"rating":4.5000000001}`:   "bad_request",
		`{"rating":"4.5"}`:          "bad_request",
		`{"rating":null}`:           "bad_request",
		`{}`:                        "bad_request",
		`{"rating":4.5,"extra":1}`:  "bad_request",
		`{"rating":4.5,"rating":4}`: "bad_request",
		`{"Rating":4.5}`:            "bad_request",
		`[4.5]`:                     "bad_request",
		`4.5`:                       "bad_request",
		``:                          "invalid_json",
		`{"rating":4.5}{}`:          "invalid_json",
		`{"rating":1e999}`:          "bad_request",
	} {
		rec := e.Do("PUT", "/api/ratings/10", body, tok)
		if rec.Code != http.StatusBadRequest || cachetest.ErrorCode(rec) != code {
			t.Errorf("%s: %d %s, want 400 %s", body, rec.Code, rec.Body.String(), code)
		}
	}
}

func TestMovieIDValidation(t *testing.T) {
	e := newEnv(t)
	_, tok := cachetest.NewUser(t, e.DB, "alice")
	for _, id := range []string{"abc", "0", "-1", "+5", "2147483648", "99999999999999999999", "1.5", "1e3", "%20"} {
		for _, method := range []string{"PUT", "DELETE"} {
			rec := e.Do(method, "/api/ratings/"+id, `{"rating":4}`, tok)
			if rec.Code != http.StatusBadRequest || !strings.Contains(rec.Body.String(), "movieId must be a positive integer") {
				t.Errorf("%s %s: %d %s", method, id, rec.Code, rec.Body.String())
			}
		}
	}
}

// ---------------------------------------------------------------- semantics

func TestPutCreatesThenUpdates(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	if rec := e.Do("PUT", "/api/ratings/4993", map[string]any{"rating": 3}, tok); rec.Code != 200 {
		t.Fatalf("create: %d %s", rec.Code, rec.Body.String())
	}
	rec := e.Do("PUT", "/api/ratings/4993", map[string]any{"rating": 4.5}, tok)
	if rec.Code != 200 {
		t.Fatalf("update: %d %s", rec.Code, rec.Body.String())
	}
	var r struct {
		MovieID int64   `json:"movieId"`
		Rating  float64 `json:"rating"`
	}
	_ = json.Unmarshal(rec.Body.Bytes(), &r)
	if r.MovieID != 4993 || r.Rating != 4.5 || countRatings(t, e.DB, uid) != 1 || cachetest.RatingsVersion(t, e.DB, uid) != 2 {
		t.Fatalf("after update: %+v count %d version %d", r, countRatings(t, e.DB, uid), cachetest.RatingsVersion(t, e.DB, uid))
	}
}

func TestUnknownMovieIsNotStored(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	rec := e.Do("PUT", "/api/ratings/999999999", map[string]any{"rating": 3}, tok)
	if rec.Code != 404 || countRatings(t, e.DB, uid) != 0 || cachetest.RatingsVersion(t, e.DB, uid) != 0 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
}

func TestDeleteBumpsOnlyWhenARowWasDeleted(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedRatings(t, e.DB, uid, ratingRow{4993, 4, 1})
	for i, want := range []int64{1, 1} {
		rec := e.Do("DELETE", "/api/ratings/4993", nil, tok)
		if rec.Code != http.StatusNoContent || rec.Body.Len() != 0 {
			t.Fatalf("delete %d: %d %s", i, rec.Code, rec.Body.String())
		}
		if v := cachetest.RatingsVersion(t, e.DB, uid); v != want {
			t.Fatalf("delete %d: version %d, want %d", i, v, want)
		}
	}
	if countRatings(t, e.DB, uid) != 0 {
		t.Fatal("row not deleted")
	}
}

// One user can neither see nor delete another user's ratings.
func TestUsersAreIsolated(t *testing.T) {
	e := newEnv(t)
	a, tokA := cachetest.NewUser(t, e.DB, "alice")
	b, tokB := cachetest.NewUser(t, e.DB, "bob")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000))
	seedRatings(t, e.DB, a, ratingRow{10, 4, 1})
	rec := e.Do("GET", "/api/ratings", nil, tokB)
	if rec.Code != 200 || strings.TrimSpace(rec.Body.String()) != `{"items":[]}` {
		t.Fatalf("bob sees %s", rec.Body.String())
	}
	if rec := e.Do("DELETE", "/api/ratings/10", nil, tokB); rec.Code != 204 {
		t.Fatal(rec.Code)
	}
	if countRatings(t, e.DB, a) != 1 || cachetest.RatingsVersion(t, e.DB, a) != 0 || cachetest.RatingsVersion(t, e.DB, b) != 0 {
		t.Fatal("bob's delete touched alice")
	}
	_ = tokA
}

func TestListNewestFirstAndCacheControl(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(1, "One", 2001), cachetest.Movie(2, "Two", 2002), cachetest.Movie(3, "Three", 2003))
	seedRatings(t, e.DB, uid, ratingRow{1, 3, 100}, ratingRow{2, 4, 300}, ratingRow{3, 5, 200})
	rec := e.Do("GET", "/api/ratings", nil, tok)
	var got struct {
		Items []struct {
			MovieID int64 `json:"movieId"`
		} `json:"items"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil || len(got.Items) != 3 ||
		got.Items[0].MovieID != 2 || got.Items[1].MovieID != 3 || got.Items[2].MovieID != 1 {
		t.Fatalf("%s", rec.Body.String())
	}
	if cc := rec.Header().Get("Cache-Control"); cc != "private, no-store" {
		t.Fatalf("Cache-Control %q", cc)
	}
}

// ---------------------------------------------------------------- 5,000 cap

func TestCap(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000), cachetest.Movie(11, "Eleven", 2000))
	seedRatings(t, e.DB, uid, manyRatings(MaxRatingsPerUser-1, 100000)...)
	if rec := e.Do("PUT", "/api/ratings/10", map[string]any{"rating": 3}, tok); rec.Code != 200 {
		t.Fatalf("5,000th: %d %s", rec.Code, rec.Body.String())
	}
	rec := e.Do("PUT", "/api/ratings/11", map[string]any{"rating": 3}, tok)
	if rec.Code != http.StatusConflict || cachetest.ErrorCode(rec) != "too_many_ratings" {
		t.Fatalf("5,001st: %d %s", rec.Code, rec.Body.String())
	}
	// Updating an existing rating at the cap is fine.
	if rec := e.Do("PUT", "/api/ratings/10", map[string]any{"rating": 4}, tok); rec.Code != 200 {
		t.Fatalf("update at cap: %d %s", rec.Code, rec.Body.String())
	}
	if n := countRatings(t, e.DB, uid); n != MaxRatingsPerUser {
		t.Fatalf("count %d", n)
	}
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != 2 {
		t.Fatalf("version %d", v)
	}
	// The list returns all 5,000 (titles for unseeded movies come from the model or a fallback).
	rec = e.Do("GET", "/api/ratings", nil, tok)
	var got struct{ Items []json.RawMessage }
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil || len(got.Items) != MaxRatingsPerUser {
		t.Fatalf("list: %d items, %v", len(got.Items), err)
	}
}

// ---------------------------------------------------------------- concurrency

// Concurrent writes: ratings_version ends equal to the number of successful
// writes (every PUT, and every DELETE that removed a row, bumps it in the
// same transaction as the write).
func TestConcurrentWritesBumpVersionExactly(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	for m := int64(1); m <= 10; m++ {
		cachetest.SeedMovies(t, e.DB, cachetest.Movie(m, fmt.Sprint("M", m), 2000))
	}
	// 20 existing ratings, each deleted exactly once below.
	seedRatings(t, e.DB, uid, manyRatings(20, 101)...)
	var puts, deletes atomic.Int64
	var wg sync.WaitGroup
	for g := 0; g < 60; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			if g < 20 {
				if r := e.Do("DELETE", fmt.Sprintf("/api/ratings/%d", 101+g), nil, tok); r.Code == 204 {
					deletes.Add(1)
				}
				return
			}
			r := e.Do("PUT", fmt.Sprintf("/api/ratings/%d", g%10+1), map[string]any{"rating": float64(g%10)/2 + 0.5}, tok)
			if r.Code == 200 {
				puts.Add(1)
			} else {
				t.Errorf("PUT: %d %s", r.Code, r.Body.String())
			}
		}(g)
	}
	wg.Wait()
	want := puts.Load() + deletes.Load()
	if v := cachetest.RatingsVersion(t, e.DB, uid); v != want || puts.Load() != 40 || deletes.Load() != 20 {
		t.Fatalf("ratings_version %d, successful writes %d (puts %d, deletes %d)", v, want, puts.Load(), deletes.Load())
	}
	if n := countRatings(t, e.DB, uid); n != 10 {
		t.Fatalf("count %d", n)
	}
}

// Concurrent new ratings near the cap: exactly enough succeed to reach 5,000.
func TestConcurrentCap(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedRatings(t, e.DB, uid, manyRatings(MaxRatingsPerUser-5, 100000)...)
	for m := int64(1); m <= 20; m++ {
		cachetest.SeedMovies(t, e.DB, cachetest.Movie(m, "M", 2000))
	}
	var ok, conflict atomic.Int64
	var wg sync.WaitGroup
	for m := 1; m <= 20; m++ {
		wg.Add(1)
		go func(m int) {
			defer wg.Done()
			r := e.Do("PUT", fmt.Sprintf("/api/ratings/%d", m), map[string]any{"rating": 2}, tok)
			switch r.Code {
			case 200:
				ok.Add(1)
			case 409:
				conflict.Add(1)
			default:
				t.Errorf("%d %s", r.Code, r.Body.String())
			}
		}(m)
	}
	wg.Wait()
	if ok.Load() != 5 || conflict.Load() != 15 || countRatings(t, e.DB, uid) != MaxRatingsPerUser || cachetest.RatingsVersion(t, e.DB, uid) != 5 {
		t.Fatalf("ok %d conflict %d count %d version %d", ok.Load(), conflict.Load(), countRatings(t, e.DB, uid), cachetest.RatingsVersion(t, e.DB, uid))
	}
}

// ---------------------------------------------------------------- degradation

// Phase 2 gate: with recs-model down, the ratings list and PUT/DELETE on
// cached movies keep working; only rating a never-seen movie returns 503.
func TestModelDown(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	cachetest.SeedMovies(t, e.DB, cachetest.Movie(10, "Ten", 2000))
	seedRatings(t, e.DB, uid, ratingRow{10, 4, 5}, ratingRow{77, 3, 4}) // 77 never cached
	e.Fake.SetDown(true)

	start := time.Now()
	rec := e.Do("GET", "/api/ratings", nil, tok)
	if rec.Code != 200 {
		t.Fatalf("list: %d %s", rec.Code, rec.Body.String())
	}
	var list struct {
		Items []struct {
			MovieID int64   `json:"movieId"`
			Title   string  `json:"title"`
			Year    *int    `json:"year"`
			Rating  float64 `json:"rating"`
		} `json:"items"`
	}
	_ = json.Unmarshal(rec.Body.Bytes(), &list)
	if len(list.Items) != 2 || list.Items[0].Title != "Ten" || list.Items[1].MovieID != 77 || list.Items[1].Title == "" || list.Items[1].Year != nil {
		t.Fatalf("list: %s", rec.Body.String())
	}
	if rec := e.Do("PUT", "/api/ratings/10", map[string]any{"rating": 5}, tok); rec.Code != 200 {
		t.Fatalf("PUT cached: %d %s", rec.Code, rec.Body.String())
	}
	if rec := e.Do("DELETE", "/api/ratings/10", nil, tok); rec.Code != 204 {
		t.Fatalf("DELETE: %d", rec.Code)
	}
	rec = e.Do("PUT", "/api/ratings/4993", map[string]any{"rating": 5}, tok)
	if rec.Code != 503 || cachetest.ErrorCode(rec) != "model_unavailable" {
		t.Fatalf("PUT uncached: %d %s", rec.Code, rec.Body.String())
	}
	if time.Since(start) > time.Second {
		t.Fatal("degraded ratings routes were slow")
	}
}

// Rating payloads never reach the logs.
func TestNoRatingPayloadInLogs(t *testing.T) {
	e := newEnv(t)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedRatings(t, e.DB, uid, ratingRow{4993, 3.5, 1})
	e.Do("PUT", "/api/ratings/4993", `{"rating":4.5}`, tok)
	e.Do("PUT", "/api/ratings/4993", `{"rating":4.3}`, tok)
	e.Do("GET", "/api/ratings", nil, tok)
	e.Fake.SetDown(true)
	e.Do("PUT", "/api/ratings/2571", `{"rating":2.5}`, tok)
	logs := e.Logs.String()
	for _, bad := range []string{`"rating"`, tok} {
		if strings.Contains(logs, bad) {
			t.Fatalf("logs contain %q:\n%s", bad, logs)
		}
	}
	if !strings.Contains(logs, `"route":"PUT /api/ratings/{movieId}"`) {
		t.Fatalf("no access log:\n%s", logs)
	}
}

// The DELETE limit is per user: a second session on the same account shares
// the bucket, and another user is unaffected.
func TestDeleteLimitIsPerUser(t *testing.T) {
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
		if rec := e.Do("DELETE", "/api/ratings/1", nil, tok); rec.Code != http.StatusNoContent {
			t.Fatalf("delete %d: %d", i, rec.Code)
		}
	}
	if rec := e.Do("DELETE", "/api/ratings/1", nil, second); rec.Code != http.StatusTooManyRequests {
		t.Errorf("second session got %d, want 429 from the per-user bucket", rec.Code)
	}
	_, bobTok := cachetest.NewUser(t, e.DB, "bob")
	if rec := e.Do("DELETE", "/api/ratings/1", nil, bobTok); rec.Code != http.StatusNoContent {
		t.Errorf("bob got %d", rec.Code)
	}
}

// Deleting an account retires its DELETE bucket for good.
func TestAccountDeletionRetiresDeleteBucket(t *testing.T) {
	var buckets *ratelimit.Keyed
	e := cachetest.NewEnv(t, nil, auth.Register, func(mux *http.ServeMux, d *app.Deps) {
		registerWith(mux, d, func() time.Time { return fixedNow }, func(k *ratelimit.Keyed) { buckets = k })
	})
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	if rec := e.Do("DELETE", "/api/ratings/1", nil, tok); rec.Code != http.StatusNoContent {
		t.Fatalf("delete rating: %d", rec.Code)
	}
	if buckets.Len() != 1 {
		t.Fatalf("buckets = %d after one delete, want 1", buckets.Len())
	}
	if rec := e.Do("DELETE", "/api/me", nil, tok); rec.Code != http.StatusNoContent {
		t.Fatalf("delete account: %d %s", rec.Code, rec.Body.String())
	}
	if buckets.Len() != 0 {
		t.Errorf("buckets = %d after the account was deleted, want 0", buckets.Len())
	}
	if ok, _ := buckets.Allow(userBucket(uid)); ok || buckets.Len() != 0 {
		t.Error("a deleted user's bucket came back")
	}
}
