#!/usr/bin/env bash
# Build every Intermission image for one commit, tagged with its short SHA:
#   intermission-{api,model,train,web,backup}:<sha>
#
#   deploy/build.sh <commit>            # build from a clean checkout of <commit>
#   deploy/build.sh --force <commit>    # rebuild even if the images exist
#
# The build always runs in a temporary `git worktree` of the commit, so local edits never leak
# into an image and the tag always matches the source. Idempotent: existing images are kept.
# Set BUILD_JOBS (default 3) to change C++ build parallelism.
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

force=0
if [[ "${1:-}" == --force ]]; then force=1; shift; fi
[[ $# -eq 1 ]] || die "usage: deploy/build.sh [--force] <commit>"

ref="$1"
git -C "$REPO_DIR" rev-parse --verify --quiet "${ref}^{commit}" >/dev/null \
  || die "commit '$ref' is not in this repository (git fetch first)"
full="$(git -C "$REPO_DIR" rev-parse "${ref}^{commit}")"
tag="${full:0:7}"

if [[ $force -eq 0 ]] && images_present "$tag"; then
  log "images for $tag already exist; nothing to build (use --force to rebuild)"
  exit 0
fi

src="$(mktemp -d "${TMPDIR:-/tmp}/intermission-build-$tag.XXXXXX")"
cleanup() {
  git -C "$REPO_DIR" worktree remove --force "$src" >/dev/null 2>&1 || rm -rf "$src"
}
trap cleanup EXIT
git -C "$REPO_DIR" worktree add --detach --quiet "$src" "$full"

for name in "${IMAGES[@]}"; do
  log "building intermission-$name:$tag"
  docker build \
    --file "$src/deploy/docker/$name.Dockerfile" \
    --build-arg "GIT_SHA=$tag" \
    --build-arg "BUILD_JOBS=${BUILD_JOBS:-3}" \
    --label "org.opencontainers.image.version=$tag" \
    --tag "intermission-$name:$tag" \
    "$src"
done

log "built:"
for name in "${IMAGES[@]}"; do
  docker image inspect "intermission-$name:$tag" --format "  {{index .Config.Labels \"org.opencontainers.image.title\"}}:$tag {{.Size}} bytes" >&2
done
