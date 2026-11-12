// Package fake is an in-memory recs-model that replays
// contracts/fixtures/model/<operationId>.json.
//
// Owner: spine (orchestrator); used by workstream J's tests.
//
// A request is matched to a fixture case by operationId plus exact method,
// path, query and body equality (bodies compare as JSON values, so key order,
// whitespace, 1 vs 1.0 and omitted optional fields do not matter). Any
// operation but movies/batch that no case matches gets its first 2xx case.
//
// A movies/batch request no case matches is checked as recs-model checks it,
// with the same codes and messages: invalid JSON, invalid UTF-8 or nesting
// deeper than 8 is invalid_json; a body that is not an object, an unexpected
// or missing field, an empty or over-200-ID list, or an ID outside
// [1, 2147483647] is bad_request. A valid one is answered from the catalog of
// every movie in the 2xx moviesBatch cases (case name "catalog"). One known
// difference: Go's decoder accepts a lone UTF-16 surrogate escape ("\ud800")
// that recs-model rejects as invalid_json.
//
// Knobs: SetDown makes every call fail with modelclient.ErrUnavailable (503
// over HTTP), SetLatency adds a delay that honours context cancellation, and
// Force pins an operation to a named case.
//
// Fake implements modelclient.Client directly; NewServer serves the same
// fixtures over HTTP for testing the real client.
package fake
