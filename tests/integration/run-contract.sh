#!/usr/bin/env bash
# Gate N1 (contract, both sides): real recs-model + real recs-api, every operation checked
# against contracts/model.yaml and contracts/public.yaml. No Docker, no network ports beyond
# one 127.0.0.1 port for recs-api.
#
#   tests/integration/run-contract.sh
#
# Builds recs-model and recs-train (release preset) if missing, installs the pinned contract
# validator (tools/contracts) if missing, then runs the Go integration tests.
set -Eeuo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"

if [[ ! -x build/release/model-service/recs-model || ! -x build/release/train/recs-train ]]; then
  cmake --preset release >/dev/null
  CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-3}" cmake --build --preset release --target recs-model recs-train
fi
[[ -d tools/contracts/node_modules ]] || (cd tools/contracts && npm ci --no-audit --no-fund)

cd tests/integration
RECS_INTEGRATION=1 go test -count=1 -v . 2>&1 | grep -v -e '^=== RUN' -e '^    --- PASS' -e '^--- PASS'
