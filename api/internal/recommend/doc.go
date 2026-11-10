// Package recommend serves /api/recommend and /api/recommend/compare: the
// recommendation LRU cache keyed by (user, ratings_version, model_version,
// model) and cold_start popularity results under 5 ratings.
//
// Owner: workstream J.
package recommend
