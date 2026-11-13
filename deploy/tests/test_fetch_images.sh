#!/usr/bin/env bash
# Unit tests for deploy/fetch-images.sh and lib.sh obtain_images (#97), with docker stubbed:
#   - a fully signed set is verified by digest with the exact signer policy, then tagged
#   - an unsigned or tampered image (cosign verify fails) is refused (exit 3): nothing tagged
#   - a missing image (pull fails) is "not available" (exit 2), before any verification
#   - a pull whose output has no digest, or more than one, is not trusted
#   - a tag failure part-way removes the tags already made
#   - obtain_images: already present → nothing; fetched → no build; not available → build;
#     refused → stops without building and records the image_verify status
#
#   deploy/tests/test_fetch_images.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else.
# shellcheck disable=SC2015,SC2016  # SC2016: stub scripts are written single-quoted on purpose
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*" >&2; fails=$((fails + 1)); }

full="$(git -C "$here" rev-parse HEAD)"
tag="${full:0:7}"
reg="ghcr.io/nickw409"

# Stub docker: records every call in $STUB_LOG, one line each.
#   STUB_PULL_FAIL=<name>       `docker pull` of that image fails
#   STUB_NO_DIGEST=<name>       its pull output has no Digest line
#   STUB_TWO_DIGESTS=<name>     its pull output has two Digest lines
#   STUB_VERIFY_FAIL=<name>     cosign verify of that image fails (unsigned / tampered)
#   STUB_TAG_FAIL=<name>        `docker tag ... intermission-<name>:...` fails
mkdir -p "$tmp/bin"
cat > "$tmp/bin/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_LOG"
digest_of() { printf 'sha256:%s' "$(printf '%s' "$1" | sha256sum | cut -c1-64)"; }
is() { [[ -n "${!1:-}" && "$2" == *"intermission-${!1}"[:@]* ]]; }
case "$1" in
  pull)
    ref="${*: -1}"; repo="${ref%:*}"; name="${repo##*/intermission-}"
    is STUB_PULL_FAIL "$ref" && { echo "Error response from daemon: manifest unknown"; exit 1; }
    echo "$name: Pulling from nickw409/intermission-$name"
    is STUB_NO_DIGEST "$ref" || echo "Digest: $(digest_of "$name")"
    is STUB_TWO_DIGESTS "$ref" && echo "Digest: sha256:$(printf '%064d' 7)"
    echo "Status: Downloaded newer image for $ref"
    exit 0 ;;
  run)
    ref="${*: -1}"
    is STUB_VERIFY_FAIL "$ref" && { echo "Error: no signatures found" >&2; exit 1; }
    exit 0 ;;
  tag)
    is STUB_TAG_FAIL "$3" && exit 1
    exit 0 ;;
esac
exit 0
EOF
chmod +x "$tmp/bin/docker"

# run_fetch [VAR=value...]: run fetch-images.sh for HEAD with the stub; sets $rc; log in $tmp/log.
run_fetch() {
  : > "$tmp/log"
  out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" "$@" "$here/fetch-images.sh" "$full" 2>&1)"
  rc=$?
}
tags_made() { grep -c '^tag ' "$tmp/log" || true; }
# The digest the stub's pull reports for an image: distinct per image, like the registry's.
dg() { printf 'sha256:%s' "$(printf '%s' "$1" | sha256sum | cut -c1-64)"; }
# removed NAME...: every pulled reference (tag and digest) of each NAME was removed again.
removed() {
  local n
  for n in "$@"; do
    grep -qx "image rm $reg/intermission-$n:$tag" "$tmp/log" || return 1
    grep -qx "image rm $reg/intermission-$n@$(dg "$n")" "$tmp/log" || return 1
  done
}

echo "== signed set: verified by digest, then tagged"
run_fetch
[[ $rc -eq 0 ]] && ok "exit 0" || bad "exit $rc: $out"
[[ "$(tags_made)" -eq 5 ]] && ok "5 images tagged" || bad "tags: $(tags_made)"
verify_lines="$(grep '^run ' "$tmp/log")"
for name in api model train web backup; do
  grep -qx "tag $reg/intermission-$name@$(dg "$name") intermission-$name:$tag" "$tmp/log" \
    && ok "$name tagged from the digest its pull reported" || bad "$name not tagged from its pulled digest"
  grep -q " $reg/intermission-$name@$(dg "$name")\$" <<<"$verify_lines" \
    && ok "$name verified at the digest its pull reported" || bad "$name not verified at its pulled digest"
done
grep -q '^image rm ' "$tmp/log" && bad "removed references after a good fetch" || ok "nothing removed on success"
[[ "$(wc -l <<<"$verify_lines")" -eq 5 ]] && ok "5 verifications" || bad "verifications: $verify_lines"
# The signer policy must stay exact: this pins it against accidental loosening.
policy=(
  "--certificate-identity https://github.com/nickw409/intermission/.github/workflows/release.yml@refs/heads/main"
  "--certificate-oidc-issuer https://token.actions.githubusercontent.com"
  "--certificate-github-workflow-repository nickw409/intermission"
  "--certificate-github-workflow-ref refs/heads/main"
  "--certificate-github-workflow-trigger push"
  "--certificate-github-workflow-sha $full"
)
for p in "${policy[@]}"; do
  [[ "$(grep -cF -- "$p" <<<"$verify_lines")" -eq 5 ]] && ok "policy: ${p%% *}" || bad "policy missing: $p"
done
grep -q -- '-regexp\|--insecure-ignore\|--allow-insecure\|--private-infrastructure' <<<"$verify_lines" \
  && bad "a loose verification flag is used" || ok "no regexp or insecure flags"
grep -v "intermission-[a-z]*@sha256:[0-9a-f]\{64\}$" <<<"$verify_lines" | grep -q . \
  && bad "a verification targets a tag, not a digest" || ok "every verification targets a digest"
grep -v 'cosign/cosign:v[0-9.]*@sha256:[0-9a-f]\{64\} verify ' <<<"$verify_lines" | grep -q . \
  && bad "cosign image not pinned by digest" || ok "cosign image pinned by digest"
for flag in --read-only "--cap-drop ALL" "--security-opt no-new-privileges:true"; do
  [[ "$(grep -cF -- "$flag" <<<"$verify_lines")" -eq 5 ]] && ok "cosign container: $flag" || bad "cosign container lacks $flag"
done

echo "== unsigned or tampered image: refused (exit 3), nothing tagged"
for name in api model backup; do
  run_fetch STUB_VERIFY_FAIL="$name"
  [[ $rc -eq 3 ]] && ok "$name: exit 3" || bad "$name: exit $rc"
  [[ "$(tags_made)" -eq 0 ]] && ok "$name: no tags" || bad "$name: $(tags_made) tags made"
  grep -q "REFUSED .*intermission-$name@" <<<"$out" && ok "$name: refusal logged" || bad "$name: no refusal: $out"
  removed api "$name" && ok "$name: pulled references removed" || bad "$name: pulled refs left"
done

echo "== missing image: not available (exit 2), never verified"
run_fetch STUB_PULL_FAIL=web
[[ $rc -eq 2 ]] && ok "exit 2" || bad "exit $rc"
[[ "$(tags_made)" -eq 0 ]] && ok "no tags" || bad "$(tags_made) tags made"
grep -q '^run .*intermission-web@' "$tmp/log" && bad "verified an image it could not pull" || ok "web never verified"
removed api model train && ok "earlier pulls removed" || bad "earlier pulls left"

echo "== pull output without exactly one digest: not trusted"
for v in STUB_NO_DIGEST STUB_TWO_DIGESTS; do
  run_fetch "$v=model"
  [[ $rc -eq 2 ]] && ok "$v: exit 2" || bad "$v: exit $rc"
  [[ "$(tags_made)" -eq 0 ]] && ok "$v: no tags" || bad "$v: $(tags_made) tags made"
  grep -q '^run .*intermission-model@' "$tmp/log" && bad "$v: verified anyway" || ok "$v: model never verified"
  removed api && grep -qx "image rm $reg/intermission-model:$tag" "$tmp/log" \
    && ok "$v: pulled references removed" || bad "$v: pulled refs left"
done

echo "== tag fails part-way: the tags already made are removed"
run_fetch STUB_TAG_FAIL=train
[[ $rc -ne 0 ]] && ok "exit non-zero" || bad "exit 0"
for name in api model train web backup; do
  grep -q "^image rm intermission-$name:$tag$" "$tmp/log" && ok "$name tag removed" || bad "$name tag left"
done
removed api model train web backup && ok "pulled references removed" || bad "pulled refs left"

echo "== unknown commit"
: > "$tmp/log"
if env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" "$here/fetch-images.sh" 0000000deadbeef >/dev/null 2>&1; then
  bad "exit 0"
else
  ok "exit non-zero"
fi
grep -q . "$tmp/log" && bad "docker called for an unknown commit" || ok "docker never called"

echo "== obtain_images"
# A copy of lib.sh in a scratch repository, next to stub fetch-images.sh and build.sh.
repo="$tmp/repo"
mkdir -p "$repo/deploy" "$tmp/data"
git -C "$repo" init -q && git -C "$repo" -c user.name=t -c user.email=t@t commit -q --allow-empty -m init
rtag="$(git -C "$repo" rev-parse --short=7 HEAD)"
cp "$here/lib.sh" "$repo/deploy/lib.sh"
for s in fetch-images build; do
  v="STUB_${s//-/_}_RC"
  printf '#!/usr/bin/env bash\necho %s >> "$STUB_LOG"\nrc="${%s:-0}"\n[[ "$rc" -eq 0 ]] && touch "$STUB_PRESENT"\nexit "$rc"\n' \
    "$s" "$v" > "$repo/deploy/$s.sh"
  chmod +x "$repo/deploy/$s.sh"
done
# docker stub for images_present: the images exist once $STUB_PRESENT exists.
mkdir -p "$tmp/bin2"
printf '#!/usr/bin/env bash\n[[ -e "$STUB_PRESENT" ]]\n' > "$tmp/bin2/docker"
chmod +x "$tmp/bin2/docker"
status_file="$tmp/data/ops/textfile/intermission_job_image_verify.prom"
run_obtain() {
  : > "$tmp/log"; rm -f "$tmp/present" "$status_file"
  out="$(env PATH="$tmp/bin2:$PATH" STUB_LOG="$tmp/log" STUB_PRESENT="$tmp/present" \
    RECS_DATA_DIR="$tmp/data" RECS_ENV_FILE="$tmp/no-env" "$@" \
    bash -c "source '$repo/deploy/lib.sh'; obtain_images HEAD" 2>&1)"
  rc=$?
}
calls() { tr '\n' ' ' < "$tmp/log"; }

: > "$tmp/log"; touch "$tmp/present"
out="$(env PATH="$tmp/bin2:$PATH" STUB_LOG="$tmp/log" STUB_PRESENT="$tmp/present" RECS_DATA_DIR="$tmp/data" \
  RECS_ENV_FILE="$tmp/no-env" bash -c "source '$repo/deploy/lib.sh'; obtain_images HEAD" 2>&1)"; rc=$?
[[ $rc -eq 0 && -z "$(calls)" ]] && ok "already present: neither fetch nor build" || bad "rc=$rc calls=$(calls)"

run_obtain
[[ $rc -eq 0 && "$(calls)" == "fetch-images " ]] && ok "verified fetch used, no build" || bad "rc=$rc calls=$(calls)"
grep -q "images for $rtag: pulled from the registry and verified" <<<"$out" && ok "fetch path logged" || bad "not logged: $out"
grep -q 'intermission_job_last_status{task="image_verify"} 0' "$status_file" 2>/dev/null \
  && ok "image_verify status cleared" || bad "image_verify status not 0"

run_obtain STUB_fetch_images_RC=2
[[ $rc -eq 0 && "$(calls)" == "fetch-images build " ]] && ok "not available: fell back to build" || bad "rc=$rc calls=$(calls)"
grep -q "images for $rtag: not available from the registry; building locally" <<<"$out" && ok "fallback logged" || bad "not logged: $out"

run_obtain STUB_fetch_images_RC=3
[[ $rc -ne 0 && "$(calls)" == "fetch-images " ]] && ok "refused: stopped, no build" || bad "rc=$rc calls=$(calls)"
grep -q "REFUSED" <<<"$out" && ok "refusal logged" || bad "not logged: $out"
grep -q 'intermission_job_last_status{task="image_verify"} 1' "$status_file" 2>/dev/null \
  && ok "image_verify status recorded (alert ImageVerifyRefused)" || bad "image_verify status not 1"

run_obtain STUB_fetch_images_RC=1
[[ $rc -ne 0 && "$(calls)" == "fetch-images " ]] && ok "unexpected fetch error: stopped, no build" || bad "rc=$rc calls=$(calls)"

run_obtain STUB_fetch_images_RC=2 STUB_build_RC=1
[[ $rc -ne 0 ]] && ok "not available and build fails: exit non-zero" || bad "exit 0"

if [[ $fails -gt 0 ]]; then
  echo "$fails check(s) failed" >&2
  exit 1
fi
echo "all fetch-images checks passed"
