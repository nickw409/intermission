// Package ratings serves GET /api/ratings and PUT/DELETE /api/ratings/{movieId}.
// Every write bumps users.ratings_version in the same transaction (so does a
// new "not interested", in package feedback); at most
// 5,000 ratings per user.
//
// Owner: workstream J.
package ratings
