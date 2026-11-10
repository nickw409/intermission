# Tailscale Funnel setup (owner only)

**Do not run any of this until the Wave 3b pre-launch security audit has passed and the owner
has signed off** (CLAUDE.md rule 12). Agents prepare this page; only the owner runs it.

Funnel hostnames get public TLS certificates, which appear in certificate-transparency logs:
the hostname is public from the first run, and scanners will probe it within days.

## Before the first run

1. The stack is deployed and healthy locally: `deploy/release.sh <sha>` passed and
   `curl -sI http://127.0.0.1:8080/` shows the security headers.
2. Nothing else on this machine is shared through Serve or Funnel: `tailscale serve status`
   and `tailscale funnel status` are empty.
3. Choose names that reveal nothing personal; they become public:
   - machine name `intermission` (admin console → Machines → Edit machine name),
   - a Tailscale-generated tailnet name (admin console → DNS → Tailnet name → "Rename
     tailnet…"; pick a generated one, not one derived from an email address).
4. Admin console → DNS: enable MagicDNS and HTTPS certificates.
5. Tailnet policy file: grant the `funnel` node attribute to this machine only, e.g.

   ```json
   "nodeAttrs": [{ "target": ["tag:intermission"], "attr": ["funnel"] }]
   ```

6. Set `RECS_SITE_ORIGIN=https://intermission.<tailnet-name>.ts.net` in `deploy/.env` and
   redeploy (`deploy/release.sh <current TAG>`): recs-api rejects mutating requests whose
   Origin differs.

## Turn it on

```bash
sudo tailscale funnel --bg 8080
tailscale funnel status    # https://intermission.<tailnet>.ts.net -> http://127.0.0.1:8080, Funnel on
```

Only port 443 is used. Do not add other ports or paths.

## Verify (from a phone on mobile data, off the home network)

1. `https://intermission.<tailnet>.ts.net/` loads; the browser shows a valid certificate.
2. Log in, rate a movie, open Compare. A `403 csrf_rejected` on login means the Host header is
   not preserved between Funnel and recs-api (see "Host header" below).
3. `https://intermission.<tailnet>.ts.net/metrics` returns 404.
4. From a laptop off the tailnet: `curl -sI https://intermission.<tailnet>.ts.net/` shows
   `strict-transport-security`, `content-security-policy`, `x-content-type-options`,
   `referrer-policy`, and no `server` header.

### Does Funnel pass the client IP?

This decides whether recs-api adds per-IP rate limits for signup and login (spec, "Rate
limits"). After step 1 above:

```bash
docker compose -f deploy/compose.yaml logs --since 5m caddy \
  | grep '"uri":"/"' | tail -n 3 \
  | python3 -c 'import sys,json; [print(json.loads(l.split("|",1)[1])["request"].get("remote_ip"), json.loads(l.split("|",1)[1])["request"]["headers"].get("X-Forwarded-For")) for l in sys.stdin]'
```

- `remote_ip` will be the Docker bridge gateway (Funnel → tailscaled → 127.0.0.1:8080 →
  docker-proxy), so it never identifies the visitor.
- If `X-Forwarded-For` holds your phone's public IP (compare with an IP-echo site on the
  phone), Funnel passes the client IP. Record that in `docs/phase-2.md`, then:
  1. in `deploy/Caddyfile`, set `trusted_proxies static <docker bridge subnet>` in the
     `servers :8080` block so Caddy takes the client IP from `X-Forwarded-For` (and overwrites
     any value a visitor sends directly),
  2. have recs-api read the client IP from the `X-Forwarded-For` value Caddy sets, and add
     the per-IP limits. Both changes need a delta audit.
- If there is no `X-Forwarded-For` (or it is a Tailscale address), Funnel does not pass the
  client IP: rely on the per-account and global caps and record that decision.

### Host header

recs-api's CSRF defence compares the browser's `Origin` with the request's `Host`, so Host must
arrive as `intermission.<tailnet>.ts.net`. Caddy preserves Host; if login fails with
`csrf_rejected`, check the Host Caddy logs (`request.host`). If Funnel rewrites it to
`127.0.0.1:8080`, set `header_up Host {http.request.header.X-Forwarded-Host}` in the `/api/*`
`reverse_proxy` block, but only after confirming Funnel always sets that header itself (and
strips any value a client sends); that change needs a delta audit.

## Uptime monitor

Create a free external HTTP monitor (e.g. UptimeRobot) for
`https://intermission.<tailnet>.ts.net/healthz`, every 5 minutes, alerting the owner by email
or push. It covers the "whole machine is off" case that local Prometheus cannot see.

## Turning it off

`take-offline.md`.
