package recommend

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/nickw409/intermission/api/internal/db"
	"github.com/nickw409/intermission/api/internal/feedback"
	"github.com/nickw409/intermission/api/internal/modelclient"
	"github.com/nickw409/intermission/api/internal/modelclient/fake"
	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

func dismiss(t *testing.T, d *db.DB, uid, movie int64) {
	t.Helper()
	tx, err := d.Writer.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback() }()
	if _, err := tx.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, ?, 'als', 'not_interested', 1)", uid, movie); err != nil {
		t.Fatal(err)
	}
	// What POST /api/feedback does in the same transaction (the end-to-end test
// below goes through the real handler).
	if _, err := tx.Exec("UPDATE users SET ratings_version = ratings_version + 1 WHERE id = ?", uid); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
}

// lastExclude returns the exclude list of the most recent recommend or
// compare call to recs-model.
func lastExclude(t *testing.T, e *cachetest.Env) []int64 {
	t.Helper()
	var out []int64
	found := false
	for _, c := range e.Fake.Calls() {
		switch c.OperationID {
		case fake.OpRecommend:
			var req modelclient.RecommendRequest
			_ = json.Unmarshal(c.Body, &req)
			out, found = req.Exclude, true
		case fake.OpCompare:
			var req modelclient.CompareRequest
			_ = json.Unmarshal(c.Body, &req)
			out, found = req.Exclude, true
		}
	}
	if !found {
		t.Fatal("no recommend or compare call reached recs-model")
	}
	return out
}

// End to end through the real POST /api/feedback handler: the dismissed
// movies are excluded, newest first after the rated ones, and the cached list
// from before is not reused.
func TestNotInterestedMoviesAreExcluded(t *testing.T) {
	e := cachetest.NewEnv(t, nil, Register, feedback.Register)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid) // rated 260, 2571, 296, 1, 318
	if rec := get(e, "/api/recommend?model=als", tok); rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if got := lastExclude(t, e); len(got) != 5 {
		t.Fatalf("exclude before dismissing: %v", got)
	}

	for _, movie := range []string{"4993", "260"} { // 260 is already rated: listed once
		if rec := e.Do("POST", "/api/feedback", `{"movieId":`+movie+`,"model":"als","action":"not_interested"}`, tok); rec.Code != 204 {
			t.Fatalf("feedback %s: %d %s", movie, rec.Code, rec.Body.String())
		}
	}
	e.Fake.ResetCalls()
	// The version bump means the cached list is not reused.
	if rec := get(e, "/api/recommend?model=als", tok); rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	got := lastExclude(t, e)
	want := []int64{260, 2571, 296, 1, 318, 4993}
	if !slices.Equal(got, want) {
		t.Errorf("exclude = %v, want %v", got, want)
	}

	// Compare uses the same list.
	e.Fake.ResetCalls()
	if rec := get(e, "/api/recommend/compare?models=als,knn", tok); rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if got := lastExclude(t, e); !slices.Equal(got, want) {
		t.Errorf("compare exclude = %v, want %v", got, want)
	}
}

// A cold-start user (under 5 ratings) gets popularity, still without the
// movies they dismissed.
func TestNotInterestedExcludedOnColdStart(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "newbie")
	dismiss(t, e.DB, uid, 318)
	if rec := get(e, "/api/recommend?model=knn&k=3", tok); rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if got := lastExclude(t, e); !slices.Equal(got, []int64{318}) {
		t.Errorf("exclude = %v", got)
	}
}

// Other users' dismissals and other feedback actions do not leak in.
func TestExcludeIsPerUserAndNotInterestedOnly(t *testing.T) {
	e := newEnv(t, nil)
	uid, tok := cachetest.NewUser(t, e.DB, "alice")
	seedAlice(t, e.DB, uid)
	bob, _ := cachetest.NewUser(t, e.DB, "bob")
	dismiss(t, e.DB, bob, 4993)
	if _, err := e.DB.Writer.Exec("INSERT INTO feedback (user_id, movie_id, model, action, ts) VALUES (?, 7, 'als', 'click', 1), (?, 8, 'als', 'impression', 1)", uid, uid); err != nil {
		t.Fatal(err)
	}
	if rec := get(e, "/api/recommend?model=als", tok); rec.Code != 200 {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
	if got := lastExclude(t, e); !slices.Equal(got, []int64{260, 2571, 296, 1, 318}) {
		t.Errorf("exclude = %v", got)
	}
}
