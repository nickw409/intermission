#!/usr/bin/env bash
# Gate: the edge (Caddy) as actually served, checked with curl rather than by reading config.
#   - the Caddyfile validates (`caddy validate` inside the web image)
#   - security headers exactly as the spec, no Server/Via header
#   - SPA fallback works; /metrics and /debug are not exposed
#   - /api/* and /healthz reach recs-api with Host preserved
#   - a 65 KB body to /api/* gets 413 from Caddy
#   - the only host binding is 127.0.0.1
#   - security audit fixes: gzip only (no zstd), unknown /assets/* is 404, recs-api's sandbox
#     CSP survives on /api/posters/*, no h2c, a stalled body gets security headers and a close
#     (its 499 status is a KNOWN Caddy limitation, reported not failed), and the access
#     log holds no query strings, headers or cookies but does hold the request id
#
# EDGE_PROJECT overrides the compose project name, EDGE_PORT the loopback port.
#
#   deploy/tests/check_edge.sh <tag>     # needs intermission-{web,api,model}:<tag>
#
# Starts a throwaway compose project (intermission-edgetest) on 127.0.0.1:${EDGE_PORT:-18081}
# with scratch data directories and named volumes; tears everything down on exit.
# pass and fail always succeed, so `cond && pass || fail` is a safe if-then-else here.
# shellcheck disable=SC2015
set -Eeuo pipefail

tag="${1:?usage: check_edge.sh <tag>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${EDGE_PORT:-18081}"
project="${EDGE_PROJECT:-intermission-edgetest}"
scratch="$(mktemp -d)"
fails=0
fail() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }
pass() { echo "  ok: $*"; }

# Scratch data dirs (read-only mounts only; everything writable is a named volume).
mkdir -p "$scratch/data/artifacts" "$scratch/data/reports" "$scratch/data/backups"
chmod -R 0755 "$scratch/data"
export TAG="$tag" RECS_HTTP_PORT="$port" RECS_DATA_DIR="$scratch/data"
dc() {
  docker compose --project-name "$project" --project-directory "$here" --env-file "$here/.env.example" \
    -f "$here/compose.yaml" -f "$here/compose.drill.yaml" "$@"
}
cleanup() {
  dc down --volumes --remove-orphans >/dev/null 2>&1 || true
  rm -rf "$scratch"
}
trap cleanup EXIT

echo "== caddy validate"
if docker run --rm --network none --read-only --cap-drop ALL "intermission-web:$tag" \
     validate --config /etc/caddy/Caddyfile --adapter caddyfile >"$scratch/validate.log" 2>&1; then
  pass "Caddyfile valid"
else
  cat "$scratch/validate.log" >&2; fail "caddy validate"
fi

echo "== start $project on 127.0.0.1:$port"
dc up -d caddy api model >/dev/null 2>&1 || { dc logs --no-color >&2; echo "FAIL: compose up" >&2; exit 1; }
base="http://127.0.0.1:$port"
host="localhost:8080" # host of RECS_SITE_ORIGIN in .env.example
for _ in $(seq 1 60); do
  curl -fs -o /dev/null --max-time 2 "$base/" && break
  sleep 1
done

bindings="$(docker port "$(dc ps -q caddy)")"
if [[ "$bindings" == "8080/tcp -> 127.0.0.1:$port" ]]; then pass "caddy bound to 127.0.0.1 only"; else fail "caddy bindings: $bindings"; fi

hdr() { # hdr FILE NAME -> value (first match, CR stripped)
  sed -n "s/^$2: *//Ip" "$1" | head -n 1 | tr -d '\r'
}
get() { # get PATH [curl args] -> status; headers in $scratch/h, body in $scratch/b
  local path="$1"; shift
  curl -sS --max-time 10 -o "$scratch/b" -D "$scratch/h" -w '%{http_code}' -H "Host: $host" "$@" "$base$path"
}

echo "== headers on /"
code="$(get /)"
[[ "$code" == 200 ]] && pass "GET / 200" || fail "GET / returned $code"
expect() { # expect NAME VALUE
  local v; v="$(hdr "$scratch/h" "$1")"
  [[ "$v" == "$2" ]] && pass "$1: $v" || fail "$1 is '$v', want '$2'"
}
expect Content-Security-Policy "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'"
expect X-Content-Type-Options nosniff
expect Referrer-Policy strict-origin-when-cross-origin
v="$(hdr "$scratch/h" Strict-Transport-Security)"
[[ "$v" =~ ^max-age=[0-9]{7,} ]] && pass "Strict-Transport-Security: $v" || fail "Strict-Transport-Security is '$v'"
for h in Server Via X-Powered-By; do
  [[ -z "$(hdr "$scratch/h" "$h")" ]] && pass "no $h header" || fail "$h header present: $(hdr "$scratch/h" "$h")"
done
grep -qi '<div id="root"\|<!doctype html' "$scratch/b" && pass "serves index.html" || fail "/ is not the SPA"

echo "== SPA fallback and internal paths"
code="$(get /evaluation/some-client-route)"
[[ "$code" == 200 ]] && grep -qi '<!doctype html' "$scratch/b" && pass "client route falls back to index.html" || fail "client route returned $code"
for p in /metrics /metrics/x /debug/pprof/ /debug/vars; do
  code="$(get "$p")"
  [[ "$code" == 404 ]] && pass "$p -> 404" || fail "$p returned $code"
  [[ -n "$(hdr "$scratch/h" Content-Security-Policy)" ]] || fail "$p response lacks security headers"
done

echo "== proxy to recs-api"
code="$(get /healthz)"
# recs-model may be absent in the test stack, so /healthz may be 503; it must come from
# recs-api (JSON), not a Caddy 502.
if [[ "$code" == 200 || "$code" == 503 ]] && grep -q '"status"' "$scratch/b"; then
  pass "/healthz proxied to recs-api ($code)"
else
  fail "/healthz returned $code: $(head -c 200 "$scratch/b")"
fi
[[ -z "$(hdr "$scratch/h" Server)" ]] && pass "no Server header on proxied response" || fail "proxied response has Server header"
[[ -n "$(hdr "$scratch/h" Content-Security-Policy)" ]] && pass "security headers on proxied response" || fail "proxied response lacks CSP"

csrf=(-H "Origin: http://$host" -H "X-Requested-With: recs" -H "Content-Type: application/json")
code="$(get /api/auth/login -X POST "${csrf[@]}" --data-binary '{"username": "x", "password": ')"
[[ "$code" == 400 ]] && pass "malformed JSON reaches recs-api -> 400 (Host preserved for CSRF)" || fail "malformed login returned $code: $(head -c 200 "$scratch/b")"

head -c 66560 /dev/zero | tr '\0' 'a' > "$scratch/big"
code="$(get /api/auth/login -X POST "${csrf[@]}" --data-binary "@$scratch/big")"
[[ "$code" == 413 ]] && pass "65 KB body -> 413" || fail "65 KB body returned $code"
# Caddy's own 413 must look like recs-api's: the public Error shape, the security headers,
# no Server header, an X-Request-ID.
grep -q '"code":"body_too_large"' "$scratch/b" && [[ "$(hdr "$scratch/h" Content-Type)" == application/json* ]] \
  && pass "413 body is the Error shape (body_too_large)" || fail "413 body is not the Error shape: $(head -c 200 "$scratch/b")"
[[ -n "$(hdr "$scratch/h" Content-Security-Policy)" && -z "$(hdr "$scratch/h" Server)" && -n "$(hdr "$scratch/h" X-Request-ID)" ]] \
  && pass "413 has security headers and X-Request-ID, no Server" || fail "413 headers: CSP '$(hdr "$scratch/h" Content-Security-Policy)', Server '$(hdr "$scratch/h" Server)', X-Request-ID '$(hdr "$scratch/h" X-Request-ID)'"
[[ "$(hdr "$scratch/h" Cache-Control)" == no-store ]] && pass "413 is Cache-Control: no-store" || fail "413 Cache-Control is '$(hdr "$scratch/h" Cache-Control)'"
head -c 65000 /dev/zero | tr '\0' 'a' > "$scratch/big"
code="$(get /api/auth/login -X POST "${csrf[@]}" --data-binary "@$scratch/big")"
[[ "$code" == 413 ]] && pass "65,000-byte body -> 413" || fail "65,000-byte body returned $code"

echo "== compression (gzip only: SA4-2, zstd disabled until Caddy ships a fixed klauspost/compress)"
asset="$(grep -o '/assets/[^"]*\.js' "$(get / >/dev/null; echo "$scratch/b")" | head -n 1 || true)"
if [[ -n "$asset" ]]; then
  get "$asset" -H 'Accept-Encoding: zstd, gzip' >/dev/null
  enc="$(hdr "$scratch/h" Content-Encoding)"
  [[ "$enc" == gzip ]] && pass "$asset served with Content-Encoding: gzip" || fail "$asset Content-Encoding is '$enc', want gzip"
  get "$asset" -H 'Accept-Encoding: zstd' >/dev/null
  enc="$(hdr "$scratch/h" Content-Encoding)"
  [[ -z "$enc" ]] && pass "zstd-only client gets an uncompressed response" || fail "zstd-only client got Content-Encoding '$enc'"
else
  fail "no JS asset referenced from index.html"
fi

echo "== unknown /assets/* is a 404, not the SPA (SA3-3)"
code="$(get /assets/does-not-exist-0123456789.js)"
[[ "$code" == 404 ]] && pass "missing asset -> 404" || fail "missing asset returned $code"
grep -qi '<!doctype html' "$scratch/b" && fail "missing asset served index.html" || pass "missing asset is not index.html"
[[ "$(hdr "$scratch/h" Cache-Control)" == *immutable* ]] && fail "missing asset cached as immutable" || pass "missing asset not cached as immutable"
[[ -n "$(hdr "$scratch/h" Content-Security-Policy)" ]] && pass "missing asset has security headers" || fail "missing asset lacks CSP"

echo "== recs-api's sandbox CSP kept on posters (SA5-7)"
code="$(get /api/posters/1)"
[[ "$code" == 200 ]] && pass "/api/posters/1 -> 200" || fail "/api/posters/1 returned $code"
expect Content-Security-Policy "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; sandbox"
get /api/me >/dev/null
expect Content-Security-Policy "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'"

echo "== HTTP/1.1 only: no h2c (SA3-1)"
ver="$(curl -s -o /dev/null --max-time 5 --http2-prior-knowledge -w '%{http_version}' -H "Host: $host" "$base/" || true)"
[[ "$ver" != 2 ]] && pass "h2c prior knowledge refused (http_version '$ver')" || fail "h2c prior knowledge accepted"
ver="$(curl -s -o /dev/null --max-time 5 --http2 -w '%{http_version}' -H "Host: $host" "$base/" || true)"
[[ "$ver" == 1.1 ]] && pass "h2c upgrade ignored, served HTTP/1.1" || fail "h2c upgrade gave http_version '$ver'"

echo "== stalled request body (SA3-5)"
# Send headers announcing 100 bytes, then 10 bytes, then stall past read_body (15 s). The
# client must get a 408 with the security headers or a clean close, never a 499.
stall="$(python3 -I - "$port" "$host" <<'PY'
import socket, sys
port, host = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=30)
s.sendall((f"POST /api/auth/login HTTP/1.1\r\nHost: {host}\r\nOrigin: http://{host}\r\n"
           "X-Requested-With: recs\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n"
           '{"username"').encode())
data = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
except socket.timeout:
    pass
print(data.decode("latin-1"))
PY
)"
status="$(head -n 1 <<<"$stall" | tr -d '\r')"
if [[ -z "$status" ]]; then
  pass "stalled body: connection closed without a response"
elif [[ "$status" == "HTTP/1.1 408"* ]]; then
  pass "stalled body -> $status"
elif [[ "$status" == "HTTP/1.1 499"* ]]; then
  # KNOWN (audit SA3-5, open): Caddy 2.11.9's reverse_proxy writes this 499 itself when the
  # request context is cancelled, bypassing handle_errors (see the Caddyfile). Reported, not
  # counted, until it can be fixed; the headers below are still enforced.
  echo "  KNOWN: stalled body answered '$status' (Caddy reverse_proxy; see Caddyfile handle_errors)"
else
  fail "stalled body answered '$status'"
fi
if [[ -n "$status" ]]; then
  grep -qi '^Content-Security-Policy:' <<<"$stall" && pass "stalled-body answer has security headers" || fail "stalled-body answer lacks CSP"
  grep -qi '^Server:' <<<"$stall" && fail "stalled-body answer has a Server header" || pass "stalled-body answer has no Server header"
  grep -qi '^Connection: close' <<<"$stall" && pass "stalled-body answer closes the connection" || fail "stalled-body answer keeps the connection open"
fi

echo "== access log: method, path, status, timing, request id only (SA3-2, SA5-2)"
get '/api/me?edgecanary=query-canary-7c1e' -H 'Cookie: session=edgetest-secret-cookie' \
  -H 'X-Edge-Canary: header-canary-5d2b' -H 'User-Agent: ua-canary-9f4a' >/dev/null
sleep 1
logs="$(dc logs --no-color caddy 2>&1)"
grep -q '"logger":"http.log.access' <<<"$logs" && pass "JSON access log lines" || fail "no JSON access log lines"
for canary in edgetest-secret-cookie query-canary-7c1e header-canary-5d2b ua-canary-9f4a; do
  grep -q "$canary" <<<"$logs" && fail "'$canary' appears in the caddy log" || pass "'$canary' not logged"
done
line="$(grep '"logger":"http.log.access' <<<"$logs" | grep '"/api/me"' | tail -n 1 || true)"
if [[ -n "$line" ]]; then
  pass "access log records the path without its query string"
  for field in '"method":"GET"' '"status":' '"duration":' '"size":' '"request_id":"'; do
    grep -q "$field" <<<"$line" && pass "access log has $field" || fail "access log lacks $field: $line"
  done
  rid="$(hdr "$scratch/h" X-Request-ID)"
  [[ -n "$rid" ]] && grep -q "\"request_id\":\"$rid\"" <<<"$line" \
    && pass "access log request_id matches X-Request-ID" || fail "request_id '$rid' not in: $line"
else
  fail "no access log line with uri \"/api/me\" (query stripped)"
fi

echo "== errors Caddy raises itself (recs-api stopped: 502)"
dc stop api >/dev/null 2>&1
code="$(get /api/me)"
[[ "$code" == 502 ]] && pass "recs-api down -> 502 from Caddy" || fail "recs-api down returned $code"
[[ -n "$(hdr "$scratch/h" Content-Security-Policy)" && -z "$(hdr "$scratch/h" Server)" && -n "$(hdr "$scratch/h" X-Request-ID)" ]] \
  && pass "502 has security headers and X-Request-ID, no Server" || fail "502 headers: CSP '$(hdr "$scratch/h" Content-Security-Policy)', Server '$(hdr "$scratch/h" Server)', X-Request-ID '$(hdr "$scratch/h" X-Request-ID)'"
[[ "$(hdr "$scratch/h" Cache-Control)" == no-store ]] && pass "502 is Cache-Control: no-store" || fail "502 Cache-Control is '$(hdr "$scratch/h" Cache-Control)'"

if [[ $fails -gt 0 ]]; then
  echo "FAIL: $fails edge check(s) failed" >&2
  exit 1
fi
echo "PASS: edge checks"
