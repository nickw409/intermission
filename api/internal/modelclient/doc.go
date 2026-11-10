// Package modelclient is recs-api's client for the internal recs-model API
// (contracts/model.yaml).
//
// Owner: workstream J. The Client interface and the request/response types in
// client.go are FROZEN by the Wave 0 spine; only the orchestrator may change
// them. Workstream J implements the real HTTP client behind NewHTTP: Unix
// socket (TCP fallback), 500 ms deadline, one retry only for idempotent reads
// and only on connection errors, X-Request-ID propagation, and mapping of
// connection errors, timeouts and 503s to ErrUnavailable.
//
// Tests use the in-memory replay fake in package modelclient/fake.
package modelclient
