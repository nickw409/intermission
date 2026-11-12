// Generates contracts/fixtures/{public,model}/*.json. Run from anywhere: node tools/contracts/gen-fixtures.mjs
// listReports is derived from contracts/fixtures/reports/, so rerun this after changing report fixtures.
// One-off generator for contracts/fixtures/{public,model}. Output is hand-maintained afterwards.
import { writeFileSync, readFileSync, readdirSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

import { fileURLToPath } from 'node:url';
const ROOT = fileURLToPath(new URL('../../contracts/fixtures', import.meta.url));
const V = '2026-11-09T21-14-03Z';
const ORIGIN = 'https://intermission.example';
const COOKIE = 'session=fixture-session-token';
const MV = { 'X-Model-Version': V };

// movie_id: [title, year, genres, tmdb, count, mean]
const M = {
  1: ['Toy Story', 1995, ['Adventure', 'Animation', 'Children', 'Comedy', 'Fantasy'], 862, 215, 3.92],
  110: ['Braveheart', 1995, ['Action', 'Drama', 'War'], 197, 237, 4.03],
  260: ['Star Wars: Episode IV - A New Hope', 1977, ['Action', 'Adventure', 'Sci-Fi'], 11, 251, 4.23],
  296: ['Pulp Fiction', 1994, ['Comedy', 'Crime', 'Drama', 'Thriller'], 680, 307, 4.2],
  318: ['Shawshank Redemption, The', 1994, ['Crime', 'Drama'], 278, 317, 4.43],
  356: ['Forrest Gump', 1994, ['Comedy', 'Drama', 'Romance', 'War'], 13, 329, 4.16],
  480: ['Jurassic Park', 1993, ['Action', 'Adventure', 'Sci-Fi', 'Thriller'], 329, 238, 3.75],
  527: ["Schindler's List", 1993, ['Drama', 'War'], 424, 220, 4.23],
  589: ['Terminator 2: Judgment Day', 1991, ['Action', 'Sci-Fi'], 280, 224, 3.97],
  593: ['Silence of the Lambs, The', 1991, ['Crime', 'Horror', 'Thriller'], 274, 279, 4.16],
  1196: ['Star Wars: Episode V - The Empire Strikes Back', 1980, ['Action', 'Adventure', 'Sci-Fi'], 1891, 211, 4.22],
  1210: ['Star Wars: Episode VI - Return of the Jedi', 1983, ['Action', 'Adventure', 'Sci-Fi'], 1892, 196, 4.14],
  2571: ['Matrix, The', 1999, ['Action', 'Sci-Fi', 'Thriller'], 603, 278, 4.19],
  2959: ['Fight Club', 1999, ['Action', 'Crime', 'Drama', 'Thriller'], 550, 218, 4.27],
  4993: ['Lord of the Rings: The Fellowship of the Ring, The', 2001, ['Adventure', 'Fantasy'], 120, 198, 4.11],
  5952: ['Lord of the Rings: The Two Towers, The', 2002, ['Adventure', 'Fantasy'], 121, 188, 4.02],
  7153: ['Lord of the Rings: The Return of the King, The', 2003, ['Action', 'Adventure', 'Drama', 'Fantasy'], 122, 185, 4.12],
};
const MISSING = 999999999;

const summary = (id) => ({ movieId: id, title: M[id][0], year: M[id][1], genres: M[id][2] });
const meta = (id) => ({ movie_id: id, title: M[id][0], year: M[id][1], genres: M[id][2], tmdb_id: M[id][3], rating_count: M[id][4], mean_rating: M[id][5] });
const movie = (id, userRating) => ({ movieId: id, title: M[id][0], year: M[id][1], genres: M[id][2], rating_count: M[id][4], mean_rating: M[id][5], tmdbId: M[id][3], user_rating: userRating });
const err = (code, message) => ({ error: { code, message } });
const t = (iso) => Date.parse(iso) / 1000;

// alice: 5 ratings (not cold), newest first
const ALICE = [
  [260, 5.0, '2026-11-09T20:41:12Z'],
  [2571, 4.5, '2026-11-09T20:40:55Z'],
  [296, 4.5, '2026-11-09T20:40:31Z'],
  [1, 4.0, '2026-11-09T20:40:02Z'],
  [318, 4.0, '2026-11-09T20:39:47Z'],
];
const aliceRatingsIn = ALICE.map(([movie_id, rating]) => ({ movie_id, rating }));
const aliceExclude = ALICE.map(([id]) => id);
// bob: 2 ratings (cold start)
const BOB_IN = [{ movie_id: 1, rating: 4.0 }, { movie_id: 260, rating: 5.0 }];

// Model-side lists (movie_id, score, because[[id, weight]])
const LISTS = {
  als: [[4993, 0.83, [[260, 0.41], [2571, 0.22]]], [1196, 0.79, [[260, 0.52]]], [2959, 0.71, [[2571, 0.38], [296, 0.3]]]],
  knn: [[1196, 0.68, [[260, 0.62]]], [1210, 0.61, [[260, 0.55]]], [4993, 0.44, [[260, 0.24], [1, 0.12]]]],
  pop: [[356, 4.12, []], [593, 4.08, []], [2959, 4.05, []]],
  popCold: [[318, 4.37, []], [356, 4.12, []], [296, 4.11, []]],
};
const modelItems = (list) => list.map(([movie_id, score, b]) => ({ movie_id, score, because: b.map(([id, weight]) => ({ movie_id: id, weight })) }));
const recItems = (list) => list.map(([id, score, b]) => ({ movieId: id, title: M[id][0], year: M[id][1], score, because: b.map(([bid, weight]) => ({ movieId: bid, title: M[bid][0], weight })) }));

const SIMILAR_KNN = [[1196, 0.62], [1210, 0.55], [2571, 0.31]];
const SEARCH_STAR = [260, 1196, 1210];
const ONBOARDING = [356, 4993, 593, 480];

function req(method, path, extra = {}) {
  return { method, path, query: extra.query ?? {}, headers: extra.headers ?? {}, ...(extra.rawBody !== undefined ? { rawBody: extra.rawBody } : {}), ...(extra.rawBodyBytes !== undefined ? { rawBodyBytes: extra.rawBodyBytes } : {}), ...(extra.body !== undefined ? { body: extra.body } : {}) };
}
function res(status, body, headers = {}) {
  return { status, headers, ...(body !== undefined ? { body } : {}) };
}
const kase = (name, request, response) => ({ name, request, response });
function write(dir, operationId, cases) {
  mkdirSync(join(ROOT, dir), { recursive: true });
  writeFileSync(join(ROOT, dir, `${operationId}.json`), JSON.stringify({ operationId, cases }, null, 2) + '\n');
}

// ---------------- model.yaml ----------------
const mreq = (method, path, extra) => req(method, path, extra);
const unavailable = res(503, err('model_unavailable', 'no verified artifact is loaded'), { 'X-Model-Version': 'none' });

write('model', 'recommend', [
  kase('ok', mreq('POST', '/v1/recommend', { body: { model: 'als', ratings: aliceRatingsIn, k: 3, exclude: aliceExclude } }),
    res(200, { model: 'als', items: modelItems(LISTS.als), ignored: 0 }, MV)),
  kase('ok_knn', mreq('POST', '/v1/recommend', { body: { model: 'knn', ratings: aliceRatingsIn, k: 3, exclude: aliceExclude } }),
    res(200, { model: 'knn', items: modelItems(LISTS.knn), ignored: 0 }, MV)),
  kase('ok_cold_start_pop', mreq('POST', '/v1/recommend', { body: { model: 'pop', ratings: BOB_IN, k: 3, exclude: [1, 260] } }),
    res(200, { model: 'pop', items: modelItems(LISTS.popCold), ignored: 0 }, MV)),
  kase('ok_ignored_unknown_movie', mreq('POST', '/v1/recommend', { body: { model: 'als', ratings: [...aliceRatingsIn, { movie_id: MISSING, rating: 3.0 }], k: 3, exclude: aliceExclude } }),
    res(200, { model: 'als', items: modelItems(LISTS.als), ignored: 1 }, MV)),
  kase('invalid_json', mreq('POST', '/v1/recommend', { rawBody: '{not json' }), res(400, err('invalid_json', 'request body is not valid JSON'), MV)),
  kase('duplicate_movie_id', mreq('POST', '/v1/recommend', { body: { model: 'als', ratings: [{ movie_id: 260, rating: 5.0 }, { movie_id: 260, rating: 4.0 }], k: 3 } }),
    res(400, err('bad_request', 'duplicate movie_id 260 in ratings'), MV)),
  kase('bad_rating_step', mreq('POST', '/v1/recommend', { body: { model: 'als', ratings: [{ movie_id: 260, rating: 4.3 }], k: 3 } }),
    res(400, err('bad_request', 'rating must be in [0.5, 5] in 0.5 steps'), MV)),
  kase('unknown_model', mreq('POST', '/v1/recommend', { body: { model: 'svd', ratings: aliceRatingsIn, k: 3 } }),
    res(400, err('unknown_model', 'unknown model "svd"'), MV)),
  kase('model_unavailable', mreq('POST', '/v1/recommend', { body: { model: 'als', ratings: aliceRatingsIn, k: 3, exclude: aliceExclude } }), unavailable),
]);

write('model', 'compare', [
  kase('ok', mreq('POST', '/v1/compare', { body: { models: ['pop', 'knn', 'als'], ratings: aliceRatingsIn, k: 3, exclude: aliceExclude } }),
    res(200, { results: [{ model: 'pop', items: modelItems(LISTS.pop) }, { model: 'knn', items: modelItems(LISTS.knn) }, { model: 'als', items: modelItems(LISTS.als) }], ignored: 0 }, MV)),
  kase('invalid_json', mreq('POST', '/v1/compare', { rawBody: '{not json' }), res(400, err('invalid_json', 'request body is not valid JSON'), MV)),
  kase('unknown_model', mreq('POST', '/v1/compare', { body: { models: ['pop', 'svd'], ratings: aliceRatingsIn, k: 3 } }),
    res(400, err('unknown_model', 'unknown model "svd"'), MV)),
  kase('model_unavailable', mreq('POST', '/v1/compare', { body: { models: ['pop', 'knn', 'als'], ratings: aliceRatingsIn, k: 3, exclude: aliceExclude } }), unavailable),
]);

write('model', 'similar', [
  kase('ok', mreq('GET', '/v1/similar/260', { query: { model: 'knn', k: '3' } }),
    res(200, { model: 'knn', movie_id: 260, items: SIMILAR_KNN.map(([movie_id, similarity]) => ({ movie_id, similarity })) }, MV)),
  kase('bad_movie_id', mreq('GET', '/v1/similar/0', { query: { model: 'knn', k: '3' } }), res(400, err('bad_request', 'movie_id must be a positive integer'), MV)),
  kase('unknown_model', mreq('GET', '/v1/similar/260', { query: { model: 'als', k: '3' } }), res(400, err('unknown_model', 'similar supports knn and content'), MV)),
  kase('not_found', mreq('GET', `/v1/similar/${MISSING}`, { query: { model: 'knn', k: '3' } }), res(404, err('not_found', `movie ${MISSING} not found`), MV)),
  kase('model_unavailable', mreq('GET', '/v1/similar/260', { query: { model: 'knn', k: '3' } }), unavailable),
]);

write('model', 'onboarding', [
  kase('ok', mreq('POST', '/v1/onboarding', { body: { n: 4, exclude: aliceExclude } }), res(200, { movie_ids: ONBOARDING }, MV)),
  kase('invalid_json', mreq('POST', '/v1/onboarding', { rawBody: '{not json' }), res(400, err('invalid_json', 'request body is not valid JSON'), MV)),
  kase('bad_n', mreq('POST', '/v1/onboarding', { body: { n: 0 } }), res(400, err('bad_request', 'n must be between 1 and 100'), MV)),
  kase('model_unavailable', mreq('POST', '/v1/onboarding', { body: { n: 4, exclude: aliceExclude } }), unavailable),
]);

write('model', 'search', [
  kase('ok', mreq('GET', '/v1/movies/search', { query: { q: 'star', limit: '3' } }), res(200, { movie_ids: SEARCH_STAR }, MV)),
  kase('empty_query', mreq('GET', '/v1/movies/search', { query: { q: '' } }), res(400, err('bad_request', 'q must be 1-100 characters'), MV)),
  kase('model_unavailable', mreq('GET', '/v1/movies/search', { query: { q: 'star', limit: '3' } }), unavailable),
]);

const batch = (name, ids) => kase(name, mreq('POST', '/v1/movies/batch', { body: { ids } }),
  res(200, { movies: ids.filter((id) => M[id]).map(meta), missing: ids.filter((id) => !M[id]) }, MV));
const idsOf = (list) => [...new Set(list.flatMap(([id, , b]) => [id, ...b.map(([bid]) => bid)]))];
write('model', 'moviesBatch', [
  batch('ok', SEARCH_STAR),
  batch('ok_with_missing', [4993, MISSING]),
  batch('recommend_als', idsOf(LISTS.als)),
  batch('compare_pop_knn_als', [...new Set([...idsOf(LISTS.pop), ...idsOf(LISTS.knn), ...idsOf(LISTS.als)])]),
  batch('cold_start_pop', idsOf(LISTS.popCold)),
  batch('similar_260', [260, ...SIMILAR_KNN.map(([id]) => id)]),
  batch('onboarding', ONBOARDING),
  batch('ratings_alice', ALICE.map(([id]) => id)),
  batch('movie_2571', [2571]),
  batch('movie_4993', [4993]),
  batch('movie_missing', [MISSING]),
  kase('empty_ids', mreq('POST', '/v1/movies/batch', { body: { ids: [] } }), res(400, err('bad_request', 'ids must contain 1-200 movie ids'), MV)),
  kase('invalid_json', mreq('POST', '/v1/movies/batch', { rawBody: '{not json' }), res(400, err('invalid_json', 'request body is not valid JSON'), MV)),
  kase('model_unavailable', mreq('POST', '/v1/movies/batch', { body: { ids: SEARCH_STAR } }), unavailable),
]);

const MODELS = { artifact_version: V, trained_at: '2026-11-09T21:14:03Z', default_model: 'als', models: ['pop', 'pop_count', 'knn', 'content', 'mf', 'als'] };
write('model', 'models', [
  kase('ok', mreq('GET', '/v1/models'), res(200, MODELS, MV)),
  kase('model_unavailable', mreq('GET', '/v1/models'), unavailable),
]);
write('model', 'version', [
  kase('ok', mreq('GET', '/v1/version'), res(200, { artifact_version: V }, MV)),
  kase('model_unavailable', mreq('GET', '/v1/version'), unavailable),
]);
write('model', 'healthz', [
  kase('ok', mreq('GET', '/healthz'), res(200, { status: 'ok' }, MV)),
  kase('unavailable', mreq('GET', '/healthz'), res(503, { status: 'unavailable' }, { 'X-Model-Version': 'none' })),
]);

// ---------------- public.yaml ----------------
const anon = {};
const sess = { Cookie: COOKIE };
const mut = { Origin: ORIGIN, 'X-Requested-With': 'recs' };
const mutSess = { ...mut, Cookie: COOKIE };
const noCsrf = { Origin: ORIGIN, Cookie: COOKIE }; // missing X-Requested-With
const P = (method, path, extra) => req(method, path, extra);
const unauthorized = res(401, err('unauthorized', 'not logged in'));
const csrf = res(403, err('csrf_rejected', 'missing or invalid X-Requested-With or Origin'));
const tooLarge = res(413, err('body_too_large', 'request body exceeds 64 KB'));
const rateLimited = res(429, err('rate_limited', 'too many requests'), { 'Retry-After': '30' });
const pubUnavailable = res(503, err('model_unavailable', 'the recommendation service is unavailable'));
const badJson = res(400, err('invalid_json', 'request body is not valid JSON'));
const setCookie = { 'Set-Cookie': 'session=fixture-session-token; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=2592000' };
const clearCookie = { 'Set-Cookie': 'session=; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=0' };
const ALICE_ME = { username: 'alice', rating_count: 5, created_at: t('2026-11-09T20:38:00Z') };

write('public', 'signup', [
  kase('ok', P('POST', '/api/auth/signup', { headers: mut, body: { username: 'alice', password: 'correct horse battery', invite_code: 'k7Qm2xVb9pLs' } }),
    res(201, { username: 'alice', rating_count: 0, created_at: t('2026-11-09T20:38:00Z') }, setCookie)),
  kase('invalid_json', P('POST', '/api/auth/signup', { headers: mut, rawBody: '{not json' }), badJson),
  kase('bad_username', P('POST', '/api/auth/signup', { headers: mut, body: { username: 'a b', password: 'correct horse battery', invite_code: 'k7Qm2xVb9pLs' } }),
    res(400, err('bad_request', 'username must be 3-32 characters of A-Z a-z 0-9 _ . -'))),
  kase('invalid_invite', P('POST', '/api/auth/signup', { headers: mut, body: { username: 'carol', password: 'correct horse battery', invite_code: 'expired-code' } }),
    res(400, err('invalid_invite', 'invite code is invalid'))),
  kase('csrf_rejected', P('POST', '/api/auth/signup', { headers: { Origin: ORIGIN }, body: { username: 'carol', password: 'correct horse battery', invite_code: 'k7Qm2xVb9pLs' } }), csrf),
  kase('username_taken', P('POST', '/api/auth/signup', { headers: mut, body: { username: 'alice', password: 'correct horse battery', invite_code: 'k7Qm2xVb9pLs' } }),
    res(409, err('username_taken', 'username is taken'))),
  kase('body_too_large', P('POST', '/api/auth/signup', { headers: mut, rawBodyBytes: 65537 }), tooLarge),
  kase('rate_limited', P('POST', '/api/auth/signup', { headers: mut, body: { username: 'carol', password: 'correct horse battery', invite_code: 'k7Qm2xVb9pLs' } }), rateLimited),
]);

write('public', 'login', [
  kase('ok', P('POST', '/api/auth/login', { headers: mut, body: { username: 'alice', password: 'correct horse battery' } }), res(200, ALICE_ME, setCookie)),
  kase('invalid_json', P('POST', '/api/auth/login', { headers: mut, rawBody: '{not json' }), badJson),
  kase('invalid_credentials', P('POST', '/api/auth/login', { headers: mut, body: { username: 'alice', password: 'wrong password!' } }),
    res(401, err('invalid_credentials', 'invalid username or password'))),
  kase('csrf_rejected', P('POST', '/api/auth/login', { headers: { Origin: ORIGIN }, body: { username: 'alice', password: 'correct horse battery' } }), csrf),
  kase('body_too_large', P('POST', '/api/auth/login', { headers: mut, rawBodyBytes: 65537 }), tooLarge),
  kase('rate_limited', P('POST', '/api/auth/login', { headers: mut, body: { username: 'alice', password: 'correct horse battery' } }), rateLimited),
]);

write('public', 'logout', [
  kase('ok', P('POST', '/api/auth/logout', { headers: mutSess }), res(204, undefined, clearCookie)),
  kase('unauthorized', P('POST', '/api/auth/logout', { headers: mut }), unauthorized),
  kase('csrf_rejected', P('POST', '/api/auth/logout', { headers: noCsrf }), csrf),
]);

write('public', 'getMe', [
  kase('ok', P('GET', '/api/me', { headers: sess }), res(200, ALICE_ME)),
  kase('unauthorized', P('GET', '/api/me'), unauthorized),
]);

write('public', 'deleteMe', [
  kase('ok', P('DELETE', '/api/me', { headers: mutSess }), res(204, undefined, clearCookie)),
  kase('unauthorized', P('DELETE', '/api/me', { headers: mut }), unauthorized),
  kase('csrf_rejected', P('DELETE', '/api/me', { headers: noCsrf }), csrf),
]);

write('public', 'searchMovies', [
  kase('ok', P('GET', '/api/movies/search', { query: { q: 'star', limit: '3' } }), res(200, { items: SEARCH_STAR.map(summary) })),
  kase('empty_query', P('GET', '/api/movies/search', { query: { q: '' } }), res(400, err('bad_request', 'q must be 1-100 characters'))),
  kase('model_unavailable', P('GET', '/api/movies/search', { query: { q: 'star', limit: '3' } }), pubUnavailable),
]);

write('public', 'getMovie', [
  kase('ok', P('GET', '/api/movies/4993'), res(200, movie(4993, null))),
  kase('ok_rated_by_caller', P('GET', '/api/movies/2571', { headers: sess }), res(200, movie(2571, 4.5))),
  kase('bad_movie_id', P('GET', '/api/movies/abc'), res(400, err('bad_request', 'movieId must be a positive integer'))),
  kase('not_found', P('GET', `/api/movies/${MISSING}`), res(404, err('not_found', 'movie not found'))),
  kase('model_unavailable', P('GET', '/api/movies/4993'), pubUnavailable),
]);

write('public', 'getSimilar', [
  kase('ok', P('GET', '/api/movies/260/similar', { query: { model: 'knn', k: '3' } }),
    res(200, { model: 'knn', model_version: V, movieId: 260, items: SIMILAR_KNN.map(([id, similarity]) => ({ movieId: id, title: M[id][0], year: M[id][1], similarity })) })),
  kase('unknown_model', P('GET', '/api/movies/260/similar', { query: { model: 'als', k: '3' } }), res(400, err('unknown_model', 'similar supports knn and content'))),
  kase('not_found', P('GET', `/api/movies/${MISSING}/similar`, { query: { model: 'knn', k: '3' } }), res(404, err('not_found', 'movie not found'))),
  kase('model_unavailable', P('GET', '/api/movies/260/similar', { query: { model: 'knn', k: '3' } }), pubUnavailable),
]);

// Overview and tagline are written for the fixture (not copied from TMDB).
const EMPTY_DETAILS = (id) => ({ movieId: id, overview: null, tagline: null, runtime_minutes: null, release_date: null, directors: [], cast: [] });
write('public', 'getMovieDetails', [
  kase('ok', P('GET', '/api/movies/4993/details'), res(200, {
    movieId: 4993,
    overview: 'A quiet hobbit inherits a dangerous ring and sets out with eight companions to destroy it before its maker can reclaim it.',
    tagline: 'One ring to rule them all.',
    runtime_minutes: 179,
    release_date: '2001-12-18',
    directors: ['Peter Jackson'],
    cast: [
      { name: 'Elijah Wood', character: 'Frodo Baggins' },
      { name: 'Ian McKellen', character: 'Gandalf' },
      { name: 'Viggo Mortensen', character: 'Aragorn' },
      { name: 'Sean Astin', character: 'Samwise Gamgee' },
      { name: 'Cate Blanchett', character: null },
    ],
  })),
  kase('ok_unavailable', P('GET', `/api/movies/${MISSING}/details`), res(200, EMPTY_DETAILS(MISSING))),
  kase('bad_movie_id', P('GET', '/api/movies/abc/details'), res(400, err('bad_request', 'movieId must be a positive integer'))),
]);

write('public', 'getOnboarding', [
  kase('ok', P('GET', '/api/onboarding', { query: { n: '4' }, headers: sess }), res(200, { items: ONBOARDING.map(summary) })),
  kase('bad_n', P('GET', '/api/onboarding', { query: { n: '0' }, headers: sess }), res(400, err('bad_request', 'n must be between 1 and 60'))),
  kase('unauthorized', P('GET', '/api/onboarding', { query: { n: '4' } }), unauthorized),
  kase('model_unavailable', P('GET', '/api/onboarding', { query: { n: '4' }, headers: sess }), pubUnavailable),
]);

write('public', 'listRatings', [
  kase('ok', P('GET', '/api/ratings', { headers: sess }),
    res(200, { items: ALICE.map(([id, rating, at]) => ({ movieId: id, title: M[id][0], year: M[id][1], rating, updated_at: t(at) })) })),
  kase('unauthorized', P('GET', '/api/ratings'), unauthorized),
]);

write('public', 'putRating', [
  kase('ok', P('PUT', '/api/ratings/4993', { headers: mutSess, body: { rating: 4.5 } }),
    res(200, { movieId: 4993, title: M[4993][0], year: M[4993][1], rating: 4.5, updated_at: t('2026-11-09T21:20:00Z') })),
  kase('invalid_json', P('PUT', '/api/ratings/4993', { headers: mutSess, rawBody: '{not json' }), badJson),
  kase('bad_rating_step', P('PUT', '/api/ratings/4993', { headers: mutSess, body: { rating: 4.3 } }), res(400, err('bad_request', 'rating must be 0.5-5.0 in 0.5 steps'))),
  kase('unauthorized', P('PUT', '/api/ratings/4993', { headers: mut, body: { rating: 4.5 } }), unauthorized),
  kase('csrf_rejected', P('PUT', '/api/ratings/4993', { headers: noCsrf, body: { rating: 4.5 } }), csrf),
  kase('not_found', P('PUT', `/api/ratings/${MISSING}`, { headers: mutSess, body: { rating: 4.5 } }), res(404, err('not_found', 'movie not found'))),
  kase('too_many_ratings', P('PUT', '/api/ratings/4993', { headers: mutSess, body: { rating: 4.5 } }), res(409, err('too_many_ratings', 'at most 5000 ratings per user'))),
  kase('body_too_large', P('PUT', '/api/ratings/4993', { headers: mutSess, rawBodyBytes: 65537 }), tooLarge),
  kase('model_unavailable', P('PUT', '/api/ratings/4993', { headers: mutSess, body: { rating: 4.5 } }), pubUnavailable),
]);

write('public', 'deleteRating', [
  kase('ok', P('DELETE', '/api/ratings/4993', { headers: mutSess }), res(204)),
  kase('bad_movie_id', P('DELETE', '/api/ratings/abc', { headers: mutSess }), res(400, err('bad_request', 'movieId must be a positive integer'))),
  kase('unauthorized', P('DELETE', '/api/ratings/4993', { headers: mut }), unauthorized),
  kase('csrf_rejected', P('DELETE', '/api/ratings/4993', { headers: noCsrf }), csrf),
]);

write('public', 'recommend', [
  kase('ok', P('GET', '/api/recommend', { query: { model: 'als', k: '3' }, headers: sess }),
    res(200, { model: 'als', model_version: V, items: recItems(LISTS.als), cold_start: false }, MV)),
  kase('ok_cold_start', P('GET', '/api/recommend', { query: { model: 'als', k: '3' }, headers: { Cookie: 'session=fixture-session-token-bob' } }),
    res(200, { model: 'pop', model_version: V, items: recItems(LISTS.popCold), cold_start: true }, MV)),
  kase('unknown_model', P('GET', '/api/recommend', { query: { model: 'svd', k: '3' }, headers: sess }), res(400, err('unknown_model', 'unknown model "svd"'))),
  kase('unauthorized', P('GET', '/api/recommend', { query: { model: 'als', k: '3' } }), unauthorized),
  kase('rate_limited', P('GET', '/api/recommend', { query: { model: 'als', k: '3' }, headers: sess }), rateLimited),
  kase('model_unavailable', P('GET', '/api/recommend', { query: { model: 'als', k: '3' }, headers: sess }), pubUnavailable),
]);

write('public', 'compare', [
  kase('ok', P('GET', '/api/recommend/compare', { query: { models: 'pop,knn,als', k: '3' }, headers: sess }),
    res(200, { model_version: V, cold_start: false, results: [['pop', LISTS.pop], ['knn', LISTS.knn], ['als', LISTS.als]].map(([model, l]) => ({ model, items: recItems(l) })) }, MV)),
  kase('too_many_models', P('GET', '/api/recommend/compare', { query: { models: 'pop,pop_count,knn,content,mf,als', k: '3' }, headers: sess }),
    res(400, err('bad_request', 'models must list 1-5 distinct model names'))),
  kase('unauthorized', P('GET', '/api/recommend/compare', { query: { models: 'pop,knn,als', k: '3' } }), unauthorized),
  kase('rate_limited', P('GET', '/api/recommend/compare', { query: { models: 'pop,knn,als', k: '3' }, headers: sess }), rateLimited),
  kase('model_unavailable', P('GET', '/api/recommend/compare', { query: { models: 'pop,knn,als', k: '3' }, headers: sess }), pubUnavailable),
]);

write('public', 'postFeedback', [
  kase('ok', P('POST', '/api/feedback', { headers: mutSess, body: { movieId: 2959, model: 'als', action: 'not_interested' } }), res(204)),
  kase('bad_action', P('POST', '/api/feedback', { headers: mutSess, body: { movieId: 2959, model: 'als', action: 'love' } }), res(400, err('bad_request', 'action must be impression, click or not_interested'))),
  kase('unauthorized', P('POST', '/api/feedback', { headers: mut, body: { movieId: 2959, model: 'als', action: 'not_interested' } }), unauthorized),
  kase('csrf_rejected', P('POST', '/api/feedback', { headers: noCsrf, body: { movieId: 2959, model: 'als', action: 'not_interested' } }), csrf),
  kase('body_too_large', P('POST', '/api/feedback', { headers: mutSess, rawBodyBytes: 65537 }), tooLarge),
  kase('rate_limited', P('POST', '/api/feedback', { headers: mutSess, body: { movieId: 2959, model: 'als', action: 'click' } }), rateLimited),
]);

write('public', 'getFeedbackStats', [
  kase('ok', P('GET', '/api/feedback/stats'), res(200, { items: [
    { model: 'als', impressions: 1240, clicks: 87, not_interested: 21, ctr: 87 / 1240 },
    { model: 'knn', impressions: 1180, clicks: 64, not_interested: 30, ctr: 64 / 1180 },
    { model: 'pop', impressions: 1310, clicks: 41, not_interested: 55, ctr: 41 / 1310 },
    { model: 'content', impressions: 0, clicks: 0, not_interested: 0, ctr: null },
  ] })),
]);

write('public', 'listModels', [
  kase('ok', P('GET', '/api/models'), res(200, MODELS)),
  kase('model_unavailable', P('GET', '/api/models'), pubUnavailable),
]);

// listReports: one summary per report fixture. Known major (1) newest first, then unknown majors with nulls.
const repDir = join(ROOT, 'reports');
const all = readdirSync(repDir).filter((f) => f.endsWith('.json')).map((f) => JSON.parse(readFileSync(join(repDir, f), 'utf8')));
const known = all.filter((r) => r.schema_version === 1).sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at));
const unknown = all.filter((r) => r.schema_version !== 1).sort((a, b) => (a.run_id < b.run_id ? -1 : 1));
const summaries = [
  ...known.map((r) => ({
    run_id: r.run_id, schema_version: r.schema_version, model: r.model, dataset: r.dataset, evaluated_on: r.evaluated_on, created_at: r.created_at, config: r.config,
    headline: { 'ndcg@10': r.metrics['ndcg@10'], 'recall@50': r.metrics['recall@50'], 'coverage@10': r.aggregates['coverage@10'] },
  })),
  ...unknown.map((r) => ({ run_id: r.run_id, schema_version: r.schema_version, model: null, dataset: null, evaluated_on: null, created_at: null, config: null, headline: null })),
];
write('public', 'listReports', [kase('ok', P('GET', '/api/reports'), res(200, { items: summaries }))]);

write('public', 'getReport', [
  kase('ok', P('GET', '/api/reports/2026-11-09T21-14-03Z_als'), res(200, { $fixtureRef: 'reports/als.json' })),
  kase('ok_pop', P('GET', '/api/reports/2026-11-09T21-00-00Z_pop'), res(200, { $fixtureRef: 'reports/pop.json' })),
  kase('ok_unknown_major', P('GET', '/api/reports/2026-12-01T00-00-00Z_pop_v2'), res(200, { $fixtureRef: 'reports/unknown_major.json' })),
  kase('bad_run_id', P('GET', '/api/reports/bad%20id'), res(400, err('bad_request', 'invalid run id'))),
  kase('not_found', P('GET', '/api/reports/2026-01-01T00-00-00Z_nope'), res(404, err('not_found', 'report not found'))),
]);

const PLACEHOLDER_SVG = '<svg xmlns="http://www.w3.org/2000/svg" width="342" height="513" viewBox="0 0 342 513"><rect width="342" height="513" fill="#2b2b2b"/></svg>';
write('public', 'getPoster', [
  kase('ok_placeholder', P('GET', '/api/posters/4993'), res(200, PLACEHOLDER_SVG, { 'Content-Type': 'image/svg+xml' })),
  kase('bad_movie_id', P('GET', '/api/posters/abc'), res(400, err('bad_request', 'movieId must be a positive integer'))),
]);

write('public', 'healthz', [
  kase('ok', P('GET', '/healthz'), res(200, { status: 'ok' })),
  kase('degraded', P('GET', '/healthz'), res(503, { status: 'degraded' })),
]);
console.log('done');
