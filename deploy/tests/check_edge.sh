#!/usr/bin/env bash
# Gate: the edge (Caddy) as actually served, checked with curl rather than by reading config.
#   - the Caddyfile validates (`caddy validate` inside the web image)
#   - security headers exactly as the spec, no Server/Via header
#   - SPA fallback works; /metrics and /debug are not exposed
#   - /api/* and /healthz reach recs-api with Host preserved
#   - a 65 KB body to /api/* gets 413 from Caddy
#   - the only host binding is 127.0.0.1
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
project=intermission-edgetest
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
head -c 65000 /dev/zero | tr '\0' 'a' > "$scratch/big"
code="$(get /api/auth/login -X POST "${csrf[@]}" --data-binary "@$scratch/big")"
[[ "$code" == 413 ]] && pass "65,000-byte body -> 413" || fail "65,000-byte body returned $code"

echo "== compression"
asset="$(grep -o '/assets/[^"]*\.js' "$(get / >/dev/null; echo "$scratch/b")" | head -n 1 || true)"
if [[ -n "$asset" ]]; then
  get "$asset" -H 'Accept-Encoding: zstd, gzip' >/dev/null
  enc="$(hdr "$scratch/h" Content-Encoding)"
  [[ "$enc" == zstd || "$enc" == gzip ]] && pass "$asset served with Content-Encoding: $enc" || fail "$asset not compressed"
else
  fail "no JS asset referenced from index.html"
fi

echo "== access log is JSON without cookies"
get /api/me -H 'Cookie: session=edgetest-secret-cookie' >/dev/null
sleep 1
logs="$(dc logs --no-color caddy 2>&1)"
grep -q '"logger":"http.log.access' <<<"$logs" && pass "JSON access log lines" || fail "no JSON access log lines"
grep -q 'edgetest-secret-cookie' <<<"$logs" && fail "cookie value appears in access log" || pass "cookie value not logged"

if [[ $fails -gt 0 ]]; then
  echo "FAIL: $fails edge check(s) failed" >&2
  exit 1
fi
echo "PASS: edge checks"
