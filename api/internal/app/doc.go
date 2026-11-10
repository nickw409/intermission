// Package app holds recs-api's process-wide configuration and the Deps
// struct handed to every package's Register function.
//
// Owner: spine (orchestrator). deps.go is a wiring file: workstreams read
// Deps but never edit it; ask the orchestrator to add a field.
package app
