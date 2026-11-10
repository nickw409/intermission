# Contracts — the single source of truth between workstreams

FROZEN after Wave 0 (`spine-v1`). Only the orchestrator changes these files; every change updates
the fixtures in the same commit and is announced to all active workstreams.

| File | Between | Consumers |
|---|---|---|
| `public.yaml` | browser ↔ recs-api | `web/src/api/schema.d.ts` (generated), Go handlers, contract tests |
| `model.yaml` | recs-api ↔ recs-model | `api/internal/modelclient`, `model-service/`, contract tests |
| `report.schema.json` | recs-train → web UI (via recs-api `/api/reports`) | `web/src/api/report.d.ts` (generated), `core/src/report/` |
| `artifact.md` | recs-train → recs-model (files) | `core/src/artifact/`, `model-service/` |
| `fixtures/public/` `fixtures/model/` | example request/response per operation | MSW mocks, Go fake model server, contract tests on both sides |
| `fixtures/reports/` | example run reports (illustrative numbers, not results) | Evaluation page, reports endpoint, validator |

## How fixtures are used
- **Go**: `api/internal/modelclient/fake` replays `fixtures/model/*.json` so recs-api is tested
  without recs-model.
- **Web**: MSW handlers are built from `fixtures/public/*.json` so the UI runs without recs-api.
- **C++**: recs-model's responses are validated against `model.yaml` schemas (exact values depend on
  the trained model, so the C++ side checks status codes, headers and shapes, not numbers).
- **CI**: `tools/contracts` validates every fixture against its schema on every push.
