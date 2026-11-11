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
limits"). The access log records no IPs or headers (security audit SA3-2), so look at the
headers on the wire instead, with a short capture in Caddy's network namespace that prints only
the forwarding headers (last part of each address masked; `Tailscale-*` values withheld):

```bash
docker build -q -t recs-capture:tmp - <<'EOF2'
FROM ubuntu:noble-20261024.1@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends tcpdump && rm -rf /var/lib/apt/lists/*
EOF2
timeout 200 docker run --rm --network container:intermission-caddy-1 --cap-drop ALL \
  --cap-add NET_RAW --cap-add NET_ADMIN recs-capture:tmp \
  timeout 120 tcpdump -Z root -i any -l -A -s 0 -q 'tcp dst port 8080' 2>/dev/null \
  | grep -aoiE '(x-forwarded-[a-z]+|tailscale-[a-z-]+): ?[^[:cntrl:]]*' \
  | sed -E 's/(tailscale-[a-z-]+):.*/\1: (withheld)/I; s/([0-9]+\.[0-9]+\.[0-9]+)\.[0-9]+/\1.x/g' \
  | sort | uniq -c
docker rmi recs-capture:tmp
```

While it runs (2 minutes), reload the site a few times from a phone on mobile data with
Tailscale disconnected. (`-Z root` is needed because the container has no capabilities to
switch to tcpdump's own user.)

**Result on 2026-11-10:** Funnel passes the client IP. Requests through Funnel carried
`X-Forwarded-For` with a public address (the phone's carrier address on the reloads; one other
address, a scanner),
`X-Forwarded-Host` (the site name), `X-Forwarded-Proto: https` and `Tailscale-Funnel-Request`.
Per-IP limits are therefore possible (finding SA1-3); the steps below are the follow-up, still to
do, and need a delta audit. Before trusting the header, the spoofing test in step 3 must show
that Funnel overwrites or appends to a client-sent `X-Forwarded-For` rather than passing it on.

- `remote_ip` will be the Docker bridge gateway (Funnel → tailscaled → 127.0.0.1:8080 →
  docker-proxy), so it never identifies the visitor.
- If `X-Forwarded-For` holds your phone's public IP (compare with an IP-echo site on the
  phone), Funnel passes the client IP. Record that in `docs/phase-2.md`, then:
  1. in `deploy/Caddyfile`, in the `servers :8080` block:

     ```
     trusted_proxies static <frontend bridge subnet>   # docker network inspect intermission_frontend
     client_ip_headers X-Forwarded-For
     trusted_proxies_strict
     ```

     `trusted_proxies_strict` makes Caddy read `X-Forwarded-For` **right to left** and take the
     first address that is not a trusted proxy. Funnel appends the real client address to
     whatever the visitor sent, so the rightmost entry is the one to believe. Without strict
     mode Caddy takes the leftmost entry, which the visitor controls.
  2. In the `/api/*` `reverse_proxy` block, add `header_up X-Real-IP {client_ip}` (Caddy always
     overwrites it), and have recs-api read the client IP from `X-Real-IP` only, never from
     `X-Forwarded-For`. Then add the per-IP limits. Both changes need a delta audit.
  3. **Spoofing test** (from the phone or any machine off the tailnet):

     ```bash
     curl -s -o /dev/null -H 'X-Forwarded-For: 203.0.113.7' -H 'X-Real-IP: 203.0.113.8' \
       https://intermission.<tailnet>.ts.net/healthz
     docker compose -f deploy/compose.yaml logs --since 2m caddy | grep healthz | tail -n 1
     ```

     The log line's `client_ip` must be the caller's real public address, never `203.0.113.7`
     or `203.0.113.8` (documentation-range addresses). Then repeat the login-rate-limit test
     sending a different spoofed `X-Forwarded-For` on every attempt: the limit must still trip,
     proving recs-api keys on the real address. If either check fails, revert step 1 and rely
     on the per-account and global caps.
- If there is no `X-Forwarded-For` (or it is a Tailscale address), Funnel does not pass the
  client IP: rely on the per-account and global caps and record that decision. Leave
  `trusted_proxies` unset so a visitor-sent `X-Forwarded-For` is never believed.

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
