#!/usr/bin/env bash
# Smoke test against a running stack, through Caddy:
#   1. GET /healthz is 200 (recs-api up and a verified model loaded)
#   2. GET / serves the SPA with the security headers
#   3. log in as the smoke-test account, GET /api/me
#   4. GET /api/recommend returns at least one item
#   5. log out
#
#   deploy/smoke.sh [base-url]     # default http://127.0.0.1:${RECS_HTTP_PORT:-8080}
#
# Credentials come from SMOKE_USERNAME / SMOKE_PASSWORD (environment, else deploy/.env).
# The password goes to curl on stdin and the session cookie lives in a 0600 temp file:
# neither appears in the process list or in the output.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

port="${RECS_HTTP_PORT:-$(env_get RECS_HTTP_PORT)}"
base="${1:-http://127.0.0.1:${port:-8080}}"
origin="${RECS_SITE_ORIGIN:-$(env_get RECS_SITE_ORIGIN)}"
user="${SMOKE_USERNAME:-$(env_get SMOKE_USERNAME)}"
pass="${SMOKE_PASSWORD:-$(env_get SMOKE_PASSWORD)}"
model="${SMOKE_MODEL:-$(env_get SMOKE_MODEL)}"; model="${model:-pop}"
[[ -n "$origin" ]] || die "RECS_SITE_ORIGIN is not set"
[[ -n "$user" && -n "$pass" ]] || die "SMOKE_USERNAME / SMOKE_PASSWORD are not set"
command -v python3 >/dev/null || die "python3 is required"

# recs-api's CSRF check compares Origin with Host, so requests carry the public Host.
host="$(origin_host "$origin")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { die "smoke: $*"; }
req() { # req METHOD PATH [curl args...] -> prints status; body in $work/body, headers in $work/headers
  local method="$1" path="$2"; shift 2
  curl -sS --max-time 10 -o "$work/body" -D "$work/headers" -w '%{http_code}' \
    -X "$method" -H "Host: $host" "$@" "$base$path"
}

code="$(req GET /healthz)" || fail "GET /healthz: connection failed"
[[ "$code" == 200 ]] || fail "GET /healthz returned $code"
log "smoke: /healthz 200"

code="$(req GET /)"
[[ "$code" == 200 ]] || fail "GET / returned $code"
grep -qi '^content-security-policy:' "$work/headers" || fail "GET / has no Content-Security-Policy header"
grep -qi '^server:' "$work/headers" && fail "GET / exposes a Server header"
log "smoke: / 200 with security headers"

# printf is a shell builtin, so the password never appears in a process's argv.
printf '%s' "$pass" | python3 -c 'import json,sys; print(json.dumps({"username": sys.argv[1], "password": sys.stdin.read()}))' \
  "$user" > "$work/login.json"
unset pass

csrf=(-H "Origin: $origin" -H "X-Requested-With: recs")
code="$(req POST /api/auth/login "${csrf[@]}" -H 'Content-Type: application/json' --data-binary "@$work/login.json")"
rm -f "$work/login.json"
[[ "$code" == 200 ]] || fail "login returned $code (does the smoke account exist? see runbooks/deploy.md)"
cookie="$(sed -n 's/^[Ss]et-[Cc]ookie: *\(session=[^;]*\).*/\1/p' "$work/headers" | tail -n 1 | tr -d '\r')"
[[ -n "$cookie" ]] || fail "login set no session cookie"
printf 'Cookie: %s\n' "$cookie" > "$work/cookie"
unset cookie
log "smoke: login 200"

code="$(req GET /api/me -H "@$work/cookie")"
[[ "$code" == 200 ]] || fail "GET /api/me returned $code"

code="$(req GET "/api/recommend?model=$model&k=10" -H "@$work/cookie")"
[[ "$code" == 200 ]] || fail "GET /api/recommend returned $code: $(head -c 300 "$work/body")"
n="$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["items"]))' "$work/body")" \
  || fail "recommend response is not the expected JSON"
[[ "$n" -ge 1 ]] || fail "recommend returned no items"
log "smoke: /api/recommend 200 with $n items (model=$model)"

code="$(req POST /api/auth/logout "${csrf[@]}" -H "@$work/cookie")"
[[ "$code" =~ ^20[04]$ ]] || fail "logout returned $code"
log "smoke: PASS ($base)"
