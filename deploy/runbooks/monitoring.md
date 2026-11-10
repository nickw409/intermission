# Monitoring (optional profile)

```bash
docker compose -f deploy/compose.yaml --profile monitoring up -d
```

starts Prometheus (127.0.0.1:9090), Grafana (127.0.0.1:3000, user `admin`, password
`GRAFANA_ADMIN_PASSWORD` from `deploy/.env`) and node-exporter (no published port). Both UIs
listen on loopback only. Reach them from another tailnet device through an SSH tunnel
(`ssh -L 3000:127.0.0.1:3000 -L 9090:127.0.0.1:9090 <host>`), never through Funnel.

The deploy scripts refuse to run while any `deploy/.env` value still starts with `change-me`
(the `.env.example` placeholders), so Grafana never starts with the example password.

## Rotate the Grafana admin password

`GRAFANA_ADMIN_PASSWORD` only seeds the admin account on Grafana's **first** start (an empty
`grafana-data` volume). Changing it in `deploy/.env` later does nothing by itself. To rotate:

```bash
read -rs -p "new Grafana admin password: " pw; echo
printf '%s' "$pw" | docker compose -f deploy/compose.yaml --profile monitoring exec -T grafana \
  /usr/share/grafana/bin/grafana cli --homepath /usr/share/grafana \
  admin reset-admin-password --password-from-stdin
unset pw
```

The password goes over stdin, so it never appears in a process list or shell history. Then
put the same value in `deploy/.env` (`GRAFANA_ADMIN_PASSWORD=`), so a rebuilt volume gets it
too, and log in again to confirm. Rotate after anyone else has seen it, and whenever the
monitoring volume is restored from somewhere else.

## What is scraped

| Target | What | Alerts |
| --- | --- | --- |
| `api:8081/metrics` (internal network) | request rate/errors/latency per route, login failures, model-client errors | `SiteDown`, `High5xxRate`, `RecommendP99High` |
| `node-exporter:9100` (internal `metrics` network) | the data filesystem (only `data/` is mounted); `data/ops/textfile/*.prom` from the nightly jobs and the drill | `DiskAbove85Percent`, `JobFailed`, `NightlyJobStale`, `RestoreDrillStale` |

Rules: `deploy/alerts/intermission.yml`, checked in CI with `promtool check rules` and
unit-tested with `promtool test rules deploy/tests/alerts.test.yml`. Firing alerts show at
http://127.0.0.1:9090/alerts.

node-exporter mounts only the data directory, read-only, with `rslave` propagation. Docker on
WSL2 refuses `rslave` for that bind ("path ... is not a shared or slave mount"); on WSL2 put
`NODE_EXPORTER_DATA_PROPAGATION=` (empty) in `deploy/.env`. The only effect is that mounts
created under `data/` after node-exporter starts are not seen until it restarts.

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
