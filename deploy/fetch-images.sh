#!/usr/bin/env bash
# Fetch the image set CI published for one commit and verify it before use (#97).
#
#   deploy/fetch-images.sh <commit>
#
# The release workflow (.github/workflows/release.yml, push to main) publishes
# ghcr.io/nickw409/intermission-{api,model,train,web,backup}:<sha7> as amd64 + arm64 image indexes
# and signs each one with cosign keyless signing. For every image this script:
#   1. pulls it from GHCR (public packages: no credentials on this host),
#   2. reads the digest Docker actually pulled (the index digest, never just the tag),
#   3. verifies that digest with cosign: a Fulcio certificate issued by GitHub Actions to
#      release.yml on refs/heads/main, triggered by a push, built from exactly <commit>,
#      and logged in Rekor,
#   4. only when all five verify, tags each one intermission-<name>:<sha7> (what release.sh uses).
#
# Any failure (no published image, unsigned, wrong identity or commit, tampered digest) exits 1
# and tags nothing; the pulled GHCR references are removed again. release.sh then builds locally
# instead (deploy/build.sh). Cosign runs from a pinned image, so the host needs only Docker.
#
# Network: ghcr.io (and its blob host), plus the Sigstore public-good instance for verification
# (tuf-repo-cdn.sigstore.dev, rekor.sigstore.dev). See deploy/runbooks/deploy.md.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ $# -eq 1 ]] || die "usage: deploy/fetch-images.sh <commit>"

REGISTRY="${RECS_IMAGE_REGISTRY:-ghcr.io/nickw409}"
# Who may have produced the images. Not overridable: loosening it would defeat the check.
SIGNER_IDENTITY="https://github.com/nickw409/intermission/.github/workflows/release.yml@refs/heads/main"
SIGNER_ISSUER="https://token.actions.githubusercontent.com"
# cosign v3.1.4, multi-arch index, pinned by digest.
COSIGN_IMAGE="ghcr.io/sigstore/cosign/cosign:v3.1.4@sha256:0f06154836d2de34f887a5c273506d52ca43edaa1d9e53e6510fa78a48f69c2d"

full="$(git -C "$REPO_DIR" rev-parse --verify --quiet --end-of-options "${1}^{commit}")" \
  || die "unknown commit '$1' (git fetch first?)"
tag="${full:0:7}"

pulled=()
cleanup_pulled() {
  local ref
  for ref in "${pulled[@]}"; do docker image rm "$ref" >/dev/null 2>&1 || true; done
}

verified=()
for name in "${IMAGES[@]}"; do
  ref="$REGISTRY/intermission-$name:$tag"
  log "pulling $ref"
  if ! docker pull --quiet "$ref" >/dev/null; then
    log "no published image $ref (not published yet, or the package is not public)"
    cleanup_pulled
    exit 1
  fi
  pulled+=("$ref")

  # The digest Docker pulled for this repository. Verifying this digest (not the tag) and tagging
  # it afterwards means a tag moved on the registry in between cannot slip through.
  digest_ref=""
  while IFS= read -r d; do
    if [[ "$d" == "$REGISTRY/intermission-$name@sha256:"* ]]; then digest_ref="$d"; fi
  done < <(docker image inspect "$ref" --format '{{range .RepoDigests}}{{println .}}{{end}}')
  if [[ ! "$digest_ref" =~ @sha256:[0-9a-f]{64}$ ]]; then
    log "cannot read the pulled digest of $ref"
    cleanup_pulled
    exit 1
  fi
  pulled+=("$digest_ref")

  log "verifying $digest_ref"
  if ! out="$(docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
      --tmpfs /tmp:size=32m --env HOME=/tmp "$COSIGN_IMAGE" verify \
      --certificate-identity "$SIGNER_IDENTITY" \
      --certificate-oidc-issuer "$SIGNER_ISSUER" \
      --certificate-github-workflow-repository nickw409/intermission \
      --certificate-github-workflow-ref refs/heads/main \
      --certificate-github-workflow-trigger push \
      --certificate-github-workflow-sha "$full" \
      "$digest_ref" 2>&1)"; then
    printf '%s\n' "$out" | tail -n 5 >&2
    log "REFUSED $digest_ref: no valid signature from the release workflow for commit $full"
    cleanup_pulled
    exit 1
  fi
  verified+=("$name=$digest_ref")
done

for entry in "${verified[@]}"; do
  name="${entry%%=*}"
  docker tag "${entry#*=}" "intermission-$name:$tag"
  log "verified intermission-$name:$tag = ${entry#*=}"
done
