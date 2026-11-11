// Package fake is an in-memory recs-model that replays
// contracts/fixtures/model/<operationId>.json.
//
// Owner: spine (orchestrator); used by workstream J's tests.
//
// A request is matched to a fixture case by operationId plus exact method,
// path, query and body equality (bodies compare as JSON values, so key order,
// whitespace, 1 vs 1.0 and omitted optional fields do not matter). When no
// case matches, a movies/batch request is answered from the catalog of every
// movie in the 2xx moviesBatch cases (case name "catalog"); any other
// operation gets its first 2xx case. Knobs: SetDown makes every
// call fail with modelclient.ErrUnavailable (503 over HTTP), SetLatency adds a
// delay that honours context cancellation, and Force pins an operation to a
// named case.
//
// Fake implements modelclient.Client directly; NewServer serves the same
// fixtures over HTTP for testing the real client.
package fake
