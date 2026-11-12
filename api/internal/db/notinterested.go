package db

import (
	"context"
	"database/sql"
)

// MaxNotInterested is how many "not interested" movies (POST /api/feedback)
// a user keeps; past it the oldest is dropped. With the 5,000-rating cap the
// recommend exclude list stays within recs-model's 10,000.
const MaxNotInterested = 5000

// Querier is what NotInterestedMovies reads through: a pool or a transaction.
type Querier interface {
	QueryContext(ctx context.Context, query string, args ...any) (*sql.Rows, error)
}

// NotInterestedMovies returns the movies uid marked "not interested", newest
// first, at most MaxNotInterested. Recommendations and onboarding leave them
// out.
func NotInterestedMovies(ctx context.Context, q Querier, uid int64) ([]int64, error) {
	rows, err := q.QueryContext(ctx,
		"SELECT movie_id FROM feedback WHERE user_id = ? AND action = 'not_interested' ORDER BY ts DESC, rowid DESC LIMIT ?",
		uid, MaxNotInterested)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}
