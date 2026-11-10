// Package posters serves GET /api/posters/{movieId}: a TMDB poster proxy (w342)
// with a size-capped LRU disk cache, returning a generated placeholder (never a
// 5xx) on any failure. The TMDB key never leaves the server.
//
// Owner: workstream J.
package posters
