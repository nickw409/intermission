// Integration tests for Intermission's services (tests/integration/README.md).
//
// The module path sits under the api module's path on purpose: Go only lets
// packages under github.com/nickw409/intermission/api import its internal/
// packages, and gate N1 drives the real recs-api model client
// (api/internal/modelclient) against the real recs-model binary. The replace
// directive points that import at the working tree's api/.
module github.com/nickw409/intermission/api/integrationtest

go 1.26.0

require github.com/nickw409/intermission/api v0.0.0

replace github.com/nickw409/intermission/api => ../../api
