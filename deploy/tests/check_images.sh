#!/usr/bin/env bash
# Gate: every runtime image runs as a non-root user and has no shell.
# Also checks that no image contains env files, keys or databases.
#
#   deploy/tests/check_images.sh <tag>     # checks intermission-{api,model,train,web,backup}:<tag>
set -Eeuo pipefail

tag="${1:?usage: check_images.sh <tag>}"
images=(api model train web backup)
shells='^(bin|usr/bin|usr/local/bin|sbin|usr/sbin)/(sh|bash|dash|ash|zsh|ksh|busybox)$'
secrets='(^|/)\.env$|\.pem$|\.key$|\.db$|\.sqlite[0-9]*$|(^|/)app\.db'
fails=0
fail() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }

for name in "${images[@]}"; do
  img="intermission-$name:$tag"
  echo "== $img"
  docker image inspect "$img" >/dev/null 2>&1 || { fail "$img not found"; continue; }

  user="$(docker image inspect "$img" --format '{{.Config.User}}')"
  uid="${user%%:*}"
  if [[ ! "$uid" =~ ^[0-9]+$ || "$uid" -eq 0 ]]; then
    fail "$img: USER is '$user' (want a numeric non-root UID)"
  else
    echo "  user $user"
  fi

  cid="$(docker create "$img")"
  files="$(docker export "$cid" | tar -t | sed 's#/$##')"
  docker rm "$cid" >/dev/null

  found="$(grep -E "$shells" <<<"$files" || true)"
  if [[ -n "$found" ]]; then fail "$img: has a shell: $(tr '\n' ' ' <<<"$found")"; else echo "  no shell binaries"; fi

  # Belt and braces: actually try to start a shell.
  if docker run --rm --network none --entrypoint /bin/sh "$img" -c true >/dev/null 2>&1; then
    fail "$img: /bin/sh runs"
  fi

  found="$(grep -E "$secrets" <<<"$files" | grep -v -E '^(etc/ssl|usr/share/ca-certificates|usr/lib/ssl)/' || true)"
  if [[ -n "$found" ]]; then fail "$img: contains suspicious files: $(tr '\n' ' ' <<<"$found")"; else echo "  no env/key/db files"; fi

  echo "  size $(docker image inspect "$img" --format '{{.Size}}') bytes"
done

if [[ $fails -gt 0 ]]; then
  echo "FAIL: $fails image check(s) failed" >&2
  exit 1
fi
echo "PASS: images run as non-root and have no shell"
