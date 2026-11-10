# Monitoring (optional profile)

```bash
docker compose -f deploy/compose.yaml --profile monitoring up -d
```

starts Prometheus (127.0.0.1:9090), Grafana (127.0.0.1:3000, user `admin`, password
`GRAFANA_ADMIN_PASSWORD` from `deploy/.env`) and node-exporter (no published port). Both UIs
listen on loopback only. Reach them from another tailnet device through an SSH tunnel
(`ssh -L 3000:127.0.0.1:3000 -L 9090:127.0.0.1:9090 <host>`), never through Funnel.

What is scraped:

| Target | What | Alerts |
| --- | --- | --- |
| `api:8081/metrics` (internal network) | request rate/errors/latency per route, login failures, model-client errors | `SiteDown`, `High5xxRate`, `RecommendP99High` |
| `node-exporter:9100` | host filesystems; `data/ops/textfile/*.prom` from the nightly jobs | `DiskAbove85Percent`, `NightlyJobFailed`, `NightlyJobStale` |

Rules: `deploy/alerts/intermission.yml` (checked in CI with `promtool check rules`). Firing
alerts show at http://127.0.0.1:9090/alerts.

## Known gaps (to reconcile)

- **Notification delivery.** Prometheus evaluates the rules but nothing pushes them to the
  owner yet. Pick a channel (e.g. ntfy, email) and either add an Alertmanager service to the
  `monitoring` profile or use Grafana alerting with the same expressions. The external
  uptime monitor (`funnel-setup.md`) already emails on "site down".
- **recs-model metrics.** recs-model listens only on its Unix socket and has no network
  (`network_mode: none`), so Prometheus cannot scrape it. Options: a loopback/internal metrics
  listener in recs-model, or recs-api relaying the model's `/metrics`.
- **recs-api metric names.** The rules assume `recs_api_http_requests_total{route,code}` and
  the histogram `recs_api_http_request_duration_seconds{route}`; adjust the rules if recs-api
  uses different names.
