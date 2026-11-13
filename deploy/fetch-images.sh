#!/usr/bin/env bash
# Fetch the image set CI published for one commit and verify it before use (#97).
#
#   deploy/fetch-images.sh <commit>
#
# The release workflow (.github/workflows/release.yml, push to main) publishes
# ghcr.io/nickw409/intermission-{api,model,train,web,backup}:<sha7> as amd64 + arm64 image indexes
# and signs each one with cosign keyless signing. For every image this script:
#   1. pulls it from GHCR (public packages: no credentials on this host),
#   2. reads the digest Docker actually pulled from `docker pull`'s own output (the index digest;
#      never the tag, and never RepoDigests, which can list several digests),
#   3. verifies that digest with cosign: a Fulcio certificate issued by GitHub Actions to
#      release.yml on refs/heads/main, triggered by a push, built from exactly <commit>,
#      and logged in Rekor,
#   4. only when all five verify, tags each one intermission-<name>:<sha7> (what release.sh uses).
#
# Exit codes (nothing is tagged on any failure; the pulled GHCR references are removed again):
#   0  all five verified and tagged
#   2  not available: an image is not published (yet), the package is not public, or the
#      registry cannot be reached. release.sh builds locally instead (deploy/build.sh).
#   3  REFUSED: an image is there but fails verification (unsigned, another signer or commit,
#      tampered). A security event, not a missing build: release.sh stops.
# Cosign runs from a pinned image, so the host needs only Docker.
#
# Network: ghcr.io (and its blob host), plus the Sigstore public-good instance for verification
# (tuf-repo-cdn.sigstore.dev, rekor.sigstore.dev). See deploy/runbooks/deploy.md.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ $# -eq 1 ]] || die "usage: deploy/fetch-images.sh <commit>"

# Where the images come from and who may have produced them. Fixed on purpose: no overrides.
REGISTRY="ghcr.io/nickw409"
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
  if ! pull_out="$(docker pull "$ref" 2>&1)"; then
    printf '%s\n' "$pull_out" | tail -n 2 >&2
    log "no published image $ref (not published yet, the package is not public, or no network)"
    cleanup_pulled
    exit 2
  fi
  pulled+=("$ref")

  # The digest this pull resolved the tag to: exactly one "Digest: sha256:..." line. Verifying
  # this digest (not the tag) and tagging it afterwards means a tag moved on the registry in
  # between cannot slip through.
  digests="$(sed -n 's/^Digest: \(sha256:[0-9a-f]\{64\}\)$/\1/p' <<<"$pull_out")"
  if [[ ! "$digests" =~ ^sha256:[0-9a-f]{64}$ ]]; then
    log "cannot read exactly one pulled digest for $ref"
    cleanup_pulled
    exit 2
  fi
  digest_ref="$REGISTRY/intermission-$name@$digests"
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
    exit 3
  fi
  verified+=("$name=$digest_ref")
done

for entry in "${verified[@]}"; do
  name="${entry%%=*}"
  if ! docker tag "${entry#*=}" "intermission-$name:$tag"; then
    # Remove the tags made so far: images_present must never see a partial set.
    for e in "${verified[@]}"; do docker image rm "intermission-${e%%=*}:$tag" >/dev/null 2>&1 || true; done
    cleanup_pulled
    die "could not tag intermission-$name:$tag"
  fi
  log "verified intermission-$name:$tag = ${entry#*=}"
done
