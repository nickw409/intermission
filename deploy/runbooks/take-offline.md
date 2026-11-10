# Take the site offline

Fastest and complete: stop Funnel. The machine stops accepting public traffic at once;
the containers keep running for debugging over the tailnet or locally.

```bash
sudo tailscale funnel --bg off      # owner only
tailscale funnel status             # must show no Funnel entries
```

Then confirm from a phone off the home network that the public URL no longer answers.

If Tailscale itself is the problem, stop the edge container instead (nothing listens on
127.0.0.1:8080 afterwards, so Funnel returns errors):

```bash
docker compose -f deploy/compose.yaml stop caddy
```

To bring the site back: fix the problem, `deploy/release.sh <sha>` (smoke test passes), then
`sudo tailscale funnel --bg 8080` (owner).
