#!/usr/bin/env bash
# Unit tests for deploy/fetch-images.sh and lib.sh obtain_images (#97), with docker stubbed:
#   - a fully signed set is verified by digest with the exact signer policy, then tagged
#   - an unsigned or tampered image (cosign verify fails) is refused: nothing is tagged
#   - a missing image (pull fails) is refused before any verification
#   - a pulled digest from another repository is refused
#   - obtain_images falls back to build.sh when fetching fails, and logs which path it took
#
#   deploy/tests/test_fetch_images.sh
# ok/bad always succeed, so `cond && ok || bad` is a safe if-then-else.
# shellcheck disable=SC2015
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
#   STUB_PULL_FAIL=<name>     `docker pull` of that image fails
#   STUB_VERIFY_FAIL=<name>   cosign verify of that image fails (unsigned / tampered)
#   STUB_FOREIGN=<name>       RepoDigests of that image name another repository
mkdir -p "$tmp/bin"
cat > "$tmp/bin/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_LOG"
digest_of() { printf 'sha256:%064d' "${#1}"; }
case "$1" in
  pull)
    ref="${*: -1}"
    [[ -n "${STUB_PULL_FAIL:-}" && "$ref" == *"intermission-$STUB_PULL_FAIL:"* ]] && exit 1
    exit 0 ;;
  image)
    if [[ "$2" == inspect ]]; then
      ref="$3"; repo="${ref%:*}"; name="${repo##*/intermission-}"
      if [[ -n "${STUB_FOREIGN:-}" && "$name" == "$STUB_FOREIGN" ]]; then
        echo "ghcr.io/evil/intermission-$name@$(digest_of "$name")"
      else
        echo "$repo@$(digest_of "$name")"
      fi
    fi
    exit 0 ;;
  run)
    ref="${*: -1}"
    [[ -n "${STUB_VERIFY_FAIL:-}" && "$ref" == *"intermission-$STUB_VERIFY_FAIL@"* ]] \
      && { echo "Error: no signatures found" >&2; exit 1; }
    exit 0 ;;
  tag) exit 0 ;;
esac
exit 0
EOF
chmod +x "$tmp/bin/docker"

# run_fetch [VAR=value...]: run fetch-images.sh for HEAD with the stub; log in $tmp/log.
run_fetch() {
  : > "$tmp/log"
  env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" "$@" "$here/fetch-images.sh" "$full" 2>&1
}
tags_made() { grep -c '^tag ' "$tmp/log" || true; }

echo "== signed set: verified by digest, then tagged"
if out="$(run_fetch)"; then ok "exit 0"; else bad "exit non-zero: $out"; fi
[[ "$(tags_made)" -eq 5 ]] && ok "5 images tagged" || bad "tags: $(tags_made)"
for name in api model train web backup; do
  grep -q "^tag $reg/intermission-$name@sha256:[0-9a-f]\{64\} intermission-$name:$tag$" "$tmp/log" \
    && ok "$name tagged from its verified digest" || bad "$name not tagged from a digest"
done
verify_lines="$(grep '^run ' "$tmp/log")"
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
grep -q -- '--certificate-identity-regexp\|--certificate-oidc-issuer-regexp\|--insecure-ignore' <<<"$verify_lines" \
  && bad "a loose verification flag is used" || ok "no regexp or insecure flags"
grep '^run ' "$tmp/log" | grep -qv "intermission-[a-z]*@sha256:[0-9a-f]\{64\}$" \
  && bad "a verification targets a tag, not a digest" || ok "every verification targets a digest"
grep '^run ' "$tmp/log" | grep -qv 'cosign/cosign:v[0-9.]*@sha256:[0-9a-f]\{64\} verify' \
  && bad "cosign image not pinned by digest" || ok "cosign image pinned by digest"

echo "== unsigned or tampered image: refused, nothing tagged"
for name in api model backup; do
  if out="$(run_fetch STUB_VERIFY_FAIL="$name")"; then bad "$name: exit 0"; else ok "$name: exit non-zero"; fi
  [[ "$(tags_made)" -eq 0 ]] && ok "$name: no tags" || bad "$name: $(tags_made) tags made"
  grep -q "REFUSED .*intermission-$name@" <<<"$out" && ok "$name: refusal logged" || bad "$name: no refusal: $out"
  grep -q "^image rm " "$tmp/log" && ok "$name: pulled references removed" || bad "$name: pulled refs left"
done

echo "== missing image: refused before verification"
if out="$(run_fetch STUB_PULL_FAIL=web)"; then bad "exit 0"; else ok "exit non-zero"; fi
[[ "$(tags_made)" -eq 0 ]] && ok "no tags" || bad "$(tags_made) tags made"
grep -q '^run .*intermission-web@' "$tmp/log" && bad "verified an image it could not pull" || ok "web never verified"

echo "== pulled digest from another repository: refused"
if out="$(run_fetch STUB_FOREIGN=model)"; then bad "exit 0"; else ok "exit non-zero"; fi
[[ "$(tags_made)" -eq 0 ]] && ok "no tags" || bad "$(tags_made) tags made"
grep -q 'evil' "$tmp/log" && bad "used the foreign digest" || ok "foreign digest never used"

echo "== unknown commit"
if out="$(env PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" "$here/fetch-images.sh" 0000000deadbeef 2>&1)"; then
  bad "exit 0"
else
  ok "exit non-zero"
fi

echo "== obtain_images: fetch first, build as fallback"
# A copy of lib.sh in a scratch repository, next to stub fetch-images.sh and build.sh.
repo="$tmp/repo"
mkdir -p "$repo/deploy"
git -C "$repo" init -q && git -C "$repo" -c user.name=t -c user.email=t@t commit -q --allow-empty -m init
rtag="$(git -C "$repo" rev-parse --short=7 HEAD)"
cp "$here/lib.sh" "$repo/deploy/lib.sh"
for s in fetch-images build; do
  printf '#!/usr/bin/env bash\necho %s >> "$STUB_LOG"\n[[ "${STUB_%s_RC:-0}" -eq 0 ]] && touch "$STUB_PRESENT"\nexit "${STUB_%s_RC:-0}"\n' \
    "$s" "${s//-/_}" "${s//-/_}" > "$repo/deploy/$s.sh"
  chmod +x "$repo/deploy/$s.sh"
done
# docker stub for images_present: images exist once $STUB_PRESENT exists.
cat > "$tmp/bin2-docker" <<'EOF'
#!/usr/bin/env bash
[[ -e "$STUB_PRESENT" ]]
EOF
mkdir -p "$tmp/bin2" && mv "$tmp/bin2-docker" "$tmp/bin2/docker" && chmod +x "$tmp/bin2/docker"
run_obtain() {
  : > "$tmp/log"; rm -f "$tmp/present"
  env PATH="$tmp/bin2:$PATH" STUB_LOG="$tmp/log" STUB_PRESENT="$tmp/present" "$@" \
    bash -c "source '$repo/deploy/lib.sh'; obtain_images HEAD" 2>&1
}
out="$(run_obtain)"; rc=$?
[[ $rc -eq 0 && "$(cat "$tmp/log")" == fetch-images ]] && ok "verified fetch used, no build" || bad "rc=$rc calls=$(cat "$tmp/log")"
grep -q "images for $rtag: pulled from the registry and verified" <<<"$out" && ok "fetch path logged" || bad "not logged: $out"
out="$(run_obtain STUB_fetch_images_RC=1)"; rc=$?
[[ $rc -eq 0 && "$(tr '\n' ' ' < "$tmp/log")" == "fetch-images build " ]] && ok "fell back to build" || bad "rc=$rc calls=$(cat "$tmp/log")"
grep -q "images for $rtag: no verified registry images; building locally" <<<"$out" && ok "fallback logged" || bad "not logged: $out"
out="$(run_obtain STUB_fetch_images_RC=1 STUB_build_RC=1)"; rc=$?
[[ $rc -ne 0 ]] && ok "both fail: exit non-zero" || bad "both failed but exit 0"

if [[ $fails -gt 0 ]]; then
  echo "$fails check(s) failed" >&2
  exit 1
fi
echo "all fetch-images checks passed"
