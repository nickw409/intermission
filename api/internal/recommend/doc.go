// Package recommend serves /api/recommend and /api/recommend/compare: the
// recommendation LRU cache keyed by (user, ratings_version, model_version,
// model, genre set, year range) and cold_start popularity results under 5
// ratings. The optional genres and year_from/year_to filters are validated
// here and forwarded to recs-model.
//
// Owner: workstream J.
package recommend
