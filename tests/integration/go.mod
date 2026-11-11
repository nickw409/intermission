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

require (
	github.com/dustin/go-humanize v1.1.0 // indirect
	github.com/google/uuid v1.6.0 // indirect
	github.com/mattn/go-isatty v0.0.24 // indirect
	github.com/ncruces/go-strftime v1.1.0 // indirect
	github.com/remyoudompheng/bigfft v0.0.0-20230129092748-24d4a6f8daec // indirect
	golang.org/x/sys v0.49.0 // indirect
	modernc.org/libc v1.80.1 // indirect
	modernc.org/mathutil v1.7.1 // indirect
	modernc.org/memory v1.12.1 // indirect
	modernc.org/sqlite v1.62.0 // indirect
)

replace github.com/nickw409/intermission/api => ../../api
