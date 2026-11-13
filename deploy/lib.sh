# Shared helpers for the deploy scripts. Source it; don't run it.
# shellcheck shell=bash

set -Eeuo pipefail
umask 077
# Fail loudly: name the script and line of any command that fails.
trap 'printf "%s ERROR: %s:%s: command failed (exit %s): %s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${BASH_SOURCE[0]:-$0}" "$LINENO" "$?" "$BASH_COMMAND" >&2' ERR

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$DEPLOY_DIR/.." && pwd)"
ENV_FILE="${RECS_ENV_FILE:-$DEPLOY_DIR/.env}"
# Deploy history (last 5 SHAs, release.sh). Overridable so test drills never touch the real one.
# shellcheck disable=SC2034  # read by release.sh, which sources this file
HISTORY_FILE="${RECS_HISTORY_FILE:-$DEPLOY_DIR/history}"
COMPOSE_FILE="$DEPLOY_DIR/compose.yaml"
IMAGES=(api model train web backup)

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

# require_env_file: the env file exists, is private (forced to 0600) and holds no placeholder
# secrets from .env.example. Every script calls it before starting or running any container.
require_env_file() {
  [[ -f "$ENV_FILE" ]] || die "$ENV_FILE not found: copy deploy/.env.example to deploy/.env and fill it in"
  local mode
  mode="$(stat -c %a "$ENV_FILE")"
  if [[ "$mode" != 600 && "$mode" != 400 ]]; then
    chmod 600 "$ENV_FILE" || die "$ENV_FILE is mode $mode and could not be made 0600"
    log "WARNING: $ENV_FILE was mode $mode; set to 0600 (it holds secrets)"
  fi
  # Placeholders: any value starting with change-me (the .env.example convention). Only the
  # key names are printed, never values.
  local placeholders
  placeholders="$(sed -n 's/^\([A-Za-z_][A-Za-z0-9_]*\)=["'\'']\{0,1\}change-me.*/\1/p' "$ENV_FILE" | sort -u | tr '\n' ' ')"
  [[ -z "$placeholders" ]] || die "placeholder values in $ENV_FILE for: ${placeholders% }; set real secrets first"
}

# acquire_lock NAME [WAIT_SECONDS]: take the one lock shared by release, backup, nightly and
# restore-drill, so they never run over each other. Waits up to WAIT_SECONDS (default 3600),
# then fails naming the holder. Scripts started by a holder (nightly -> backup.sh) inherit it.
acquire_lock() {
  local name="$1" wait="${2:-3600}" lock="${RECS_LOCK_FILE:-$DEPLOY_DIR/ops.lock}"
  [[ -n "${INTERMISSION_LOCK_HELD:-}" ]] && return 0
  exec 9<>"$lock"
  if ! flock -w "$wait" 9; then
    die "$name: another deploy operation is running ($(cat "$lock" 2>/dev/null || echo unknown)); waited ${wait}s"
  fi
  printf '%s pid %s since %s\n' "$name" "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$lock"
  export INTERMISSION_LOCK_HELD="$name"
}

# env_get KEY: value of KEY in the env file (last assignment wins), empty if unset.
env_get() {
  local key="$1"
  [[ -f "$ENV_FILE" ]] || return 0
  sed -n "s/^${key}=//p" "$ENV_FILE" | tail -n 1
}

# env_set KEY VALUE: replace (or append) KEY=VALUE in the env file atomically, keeping mode 0600.
env_set() {
  local key="$1" value="$2" tmp
  tmp="$(mktemp "$ENV_FILE.XXXXXX")"
  if grep -q "^${key}=" "$ENV_FILE"; then
    awk -v k="$key" -v v="$value" 'BEGIN{FS=OFS="="} $1==k {print k "=" v; next} {print}' "$ENV_FILE" > "$tmp"
  else
    cat "$ENV_FILE" > "$tmp"
    printf '%s=%s\n' "$key" "$value" >> "$tmp"
  fi
  chmod 600 "$tmp"
  mv -f "$tmp" "$ENV_FILE"
}

# compose ARGS...: docker compose for the production project.
compose() {
  docker compose --project-directory "$DEPLOY_DIR" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"
}

# short_sha REF: 7-character SHA of a commit that exists in this repository. Images loaded
# from a CI artifact still need their commit fetched first (git fetch), so a tag always maps
# to source that can be inspected and rebuilt.
short_sha() {
  local ref="$1" sha
  sha="$(git -C "$REPO_DIR" rev-parse --verify --quiet --end-of-options "${ref}^{commit}")" \
    || die "unknown commit '$ref' (git fetch first?)"
  printf '%s\n' "${sha:0:7}"
}

images_present() {
  local tag="$1" name
  for name in "${IMAGES[@]}"; do
    docker image inspect "intermission-$name:$tag" >/dev/null 2>&1 || return 1
  done
}

# The GitHub repository whose CI and published images release.sh trusts.
GITHUB_REPO="nickw409/intermission"
# The workflows that gate a release: every push to main must pass both. release.yml's push run
# also publishes and signs the image set (its publish jobs run on every push to main once the
# gates pass), so a successful one means a signed set exists; fetch-images.sh still verifies it.
# Other workflows (the scheduled security scan, which attaches to whatever main's head is) and
# check runs posted by other apps do not count.
GATE_WORKFLOWS=".github/workflows/ci.yml .github/workflows/release.yml"

# commit_green SHA: whether the latest push-triggered run on main of every GATE_WORKFLOWS
# workflow for SHA completed successfully. GitHub's public Actions API, no credentials (60
# requests an hour per IP; one per call). Exit 0: green. 1: not (reason logged). 2: the API could
# not be asked or answered something unreadable.
commit_green() {
  local sha="$1" json reason rc=0
  json="$(curl -fsS --max-time 15 -H 'Accept: application/vnd.github+json' \
    "https://api.github.com/repos/$GITHUB_REPO/actions/runs?head_sha=$sha&event=push&branch=main&per_page=100" 2>&1)" \
    || { log "GitHub API request for ${sha:0:7} failed: $(printf '%s' "${json:0:200}" | tr -cd '[:print:]')"; return 2; }
  reason="$(python3 -c '
import json, sys
sha, gates = sys.argv[1], sys.argv[2].split()
def clean(x):  # API text goes into logs: printable characters only
    return "".join(c for c in str(x) if c.isprintable())[:80]
try:
    d = json.loads(sys.stdin.read())
    runs = d["workflow_runs"]
except Exception as e:
    print("unreadable API response: %s" % clean(e)); sys.exit(2)
if d.get("total_count", len(runs)) > len(runs):
    print("more workflow runs than one page"); sys.exit(1)
problems = []
for path in gates:
    mine = [r for r in runs if r.get("path") == path and r.get("head_sha") == sha
            and r.get("event") == "push" and r.get("head_branch") == "main"]
    name = path.rsplit("/", 1)[-1]
    if not mine:
        problems.append("%s has not run" % name); continue
    r = max(mine, key=lambda r: r["id"])  # the newest run; re-runs update it in place
    if r.get("status") != "completed":
        problems.append("%s still running" % name)
    elif r.get("conclusion") != "success":
        problems.append("%s %s" % (name, clean(r.get("conclusion"))))
if problems:
    print("; ".join(problems)); sys.exit(1)
' "$sha" "$GATE_WORKFLOWS" <<<"$json")" || rc=$?
  case "$rc" in
    0) return 0 ;;
    1) log "${sha:0:7}: $reason"; return 1 ;;
    *) log "${sha:0:7}: $reason"; return 2 ;;
  esac
}

# latest_green_main: print the newest commit on origin/main (first parent, after a git fetch)
# that commit_green accepts, looking back at most 20 commits. Dies if none qualifies, or if the
# API cannot be asked: it never falls back to an older commit because of an API failure.
latest_green_main() {
  local sha rc shas
  git -C "$REPO_DIR" fetch --quiet origin || die "git fetch failed"
  shas="$(git -C "$REPO_DIR" rev-list --first-parent --max-count=20 origin/main)" \
    || die "cannot list commits on origin/main"
  for sha in $shas; do
    rc=0
    commit_green "$sha" || rc=$?
    case "$rc" in
      0) printf '%s\n' "$sha"; return 0 ;;
      1) ;;
      *) die "cannot check CI status on GitHub; deploy a SHA explicitly or retry later" ;;
    esac
  done
  die "none of the last 20 commits on origin/main passed ci.yml and release.yml"
}

# moves_forward NEW PREVIOUS: whether deploying commit NEW keeps or advances PREVIOUS (the running
# TAG): PREVIOUS is NEW or an ancestor of it. Exit 0: yes, or nothing runs yet (empty PREVIOUS).
# 1: NEW is older than PREVIOUS or on another line of history. 2: PREVIOUS is not exactly one
# commit in this repository, so it cannot be decided (fail closed).
moves_forward() {
  local new="$1" prev
  [[ -n "${2:-}" ]] || return 0
  prev="$(commit_id "$2")" || return 2
  if git -C "$REPO_DIR" merge-base --is-ancestor "$prev" "$new"; then return 0; fi
  return 1
}

# obtain_images COMMIT [--verified-only]: make intermission-*:<sha7> present. Order: already
# loaded (trusted as is: only the docker group, which is root-equivalent, can load images); else
# the CI-published set from GHCR, verified (fetch-images.sh); else, only when nothing is
# published, a local build (build.sh). With --verified-only there is no local build: the images
# must come from the registry and pass verification. An image that is published but fails
# verification stops the release: that is a security event, not a missing build. Logs which path
# it took. Dies if images are still missing.
obtain_images() {
  local tag rc=0 verified_only=0
  [[ "${2:-}" == --verified-only ]] && verified_only=1
  tag="$(short_sha "$1")"
  if images_present "$tag"; then
    log "images for $tag: already present locally"
    return 0
  fi
  "$DEPLOY_DIR/fetch-images.sh" "$1" || rc=$?
  case "$rc" in
    0) job_status image_verify 0
       log "images for $tag: pulled from the registry and verified" ;;
    2) if [[ $verified_only -eq 1 ]]; then
         die "images for $tag: not available from the registry, and this release only deploys verified registry images (name the SHA to allow a local build)"
       fi
       log "images for $tag: not available from the registry; building locally (deploy/build.sh)"
       "$DEPLOY_DIR/build.sh" "$1" ;;
    3) job_status image_verify 1
       die "images for $tag: REFUSED, the published images failed signature verification. Nothing was deployed and nothing will be built. See deploy/runbooks/deploy.md, \"Images from the registry\"" ;;
    *) die "images for $tag: fetch-images.sh failed (exit $rc)" ;;
  esac
  images_present "$tag" || die "images for $tag are still missing"
}

# wait_healthy URL HOST SECONDS: poll URL until it returns 200.
wait_healthy() {
  local url="$1" host="$2" deadline=$((SECONDS + $3))
  until curl -fs -o /dev/null --max-time 3 -H "Host: $host" "$url"; do
    (( SECONDS < deadline )) || return 1
    sleep 2
  done
}

# data_dir: the host data directory, as compose resolves RECS_DATA_DIR (relative to deploy/).
data_dir() {
  local d="${RECS_DATA_DIR:-$(env_get RECS_DATA_DIR)}"
  d="${d:-../data}"
  [[ "$d" == /* ]] || d="$DEPLOY_DIR/$d"
  printf '%s\n' "$d"
}

# require_data_dirs DIR...: fail unless each data subdirectory exists (otherwise Docker would
# create it owned by root and the non-root containers could not write it).
require_data_dirs() {
  local d base
  base="$(data_dir)"
  for d in "$@"; do
    [[ -d "$base/$d" ]] || die "$base/$d is missing: run sudo deploy/init-data.sh"
  done
}

# check_artifact TAG: ask TAG's recs-model whether it would serve the artifact production's
# CURRENT names (recs-model --check, run as the compose `model-check` service: the model image,
# user and artifacts read-only, no network, no model socket). A shell TAG overrides the env
# file's, so the running services are untouched. Prints the check's version=, git_sha= and
# models= lines on stdout. Exit codes:
#   0  the artifact loads with TAG's recs-model (or the check was skipped: nothing printed)
#   3  that recs-model would refuse the artifact: a retrain is needed first
#   4  there is no CURRENT pointer
#   other: the check could not be run (Docker or compose error, the reason is logged)
# Images built before --check existed (a rollback to an older release) cannot be asked; only for
# them is the check skipped with a warning. Any failure to even start the image stops instead.
check_artifact() {
  local help rc=0
  if ! help="$(TAG="$1" compose run --rm --no-deps --pull never -T model-check --help 2>&1)"; then
    log "ERROR: could not start recs-model:$1 to check the artifact:"
    printf '%s\n' "$help" >&2
    return 2
  fi
  if [[ "$help" != *--check* ]]; then
    log "WARNING: recs-model:$1 predates --check; skipping the artifact preflight"
    return 0
  fi
  TAG="$1" compose run --rm --no-deps --pull never -T model-check || rc=$?
  return "$rc"
}

# What decides the contents of an artifact: the training code and its configs, plus what builds
# it (dependency pins, compiler flags, the train image). Broad on purpose (all of core/, not just
# the models): an unneeded retrain costs machine time, a missed one leaves the site on a model the
# code has moved past.
TRAINING_PATHS=(core train configs cmake CMakeLists.txt CMakePresets.json deploy/docker/train.Dockerfile)

# commit_id PREFIX: the full id of the one commit whose id starts with PREFIX (7-40 lowercase
# hex), else nothing and exit 1. Looked up as an object id only, so a branch or tag that happens
# to be named like a hex string is never picked instead.
commit_id() {
  local prefix="$1" obj found=""
  [[ "$prefix" =~ ^[0-9a-f]{7,40}$ ]] || return 1
  for obj in $(git -C "$REPO_DIR" rev-parse --disambiguate="$prefix" 2>/dev/null); do
    [[ "$(git -C "$REPO_DIR" cat-file -t "$obj" 2>/dev/null)" == commit ]] || continue
    [[ -z "$found" ]] || return 1  # ambiguous
    found="$obj"
  done
  [[ -n "$found" ]] || return 1
  printf '%s\n' "$found"
}

# training_changed FROM TO: whether TRAINING_PATHS differ between commit FROM (the git_sha an
# artifact was trained at) and commit TO (the release). "Differ", not "newer": on a rollback the
# artifact can be the newer side, and a retrain with the older code is still what matches it.
# Exit 0: they differ, a retrain is recommended; 1: identical; 2: cannot tell (FROM is not
# exactly one commit in this repository, or git failed).
training_changed() {
  local from to rc=0
  from="$(commit_id "$1")" || return 2
  to="$(git -C "$REPO_DIR" rev-parse --verify --quiet --end-of-options "${2}^{commit}")" || return 2
  git -C "$REPO_DIR" diff --quiet "$from" "$to" -- "${TRAINING_PATHS[@]}" || rc=$?
  case "$rc" in
    0) return 1 ;;
    1) return 0 ;;
    *) return 2 ;;
  esac
}

# origin_host ORIGIN: host[:port] part of scheme://host[:port].
origin_host() { local o="${1#*://}"; printf '%s\n' "${o%%/*}"; }

# job_status JOB CODE: record a job result for node-exporter's textfile collector
# (alerts: JobFailed, NightlyJobStale, RestoreDrillStale; release.sh's artifact_check and
# artifact_freshness: RetrainRequired, RetrainRecommended). Never fails the caller.
job_status() {
  local job="$1" code="$2" dir now
  dir="$(data_dir)/ops/textfile"
  now="$(date +%s)"
  {
    mkdir -p "$dir" && chmod 755 "$dir"
    local f="$dir/intermission_job_${job}.prom" prev_ok=0
    if [[ -f "$f" ]]; then
      prev_ok="$(sed -n 's/^intermission_job_last_success_timestamp_seconds{task="[a-z_]*"} //p' "$f")"
    fi
    [[ "$code" == 0 ]] && prev_ok="$now"
    local tmp
    tmp="$(mktemp "$dir/.tmp.XXXXXX")"
    {
      echo "# HELP intermission_job_last_status Exit status of the last run (0 = ok)."
      echo "# TYPE intermission_job_last_status gauge"
      echo "intermission_job_last_status{task=\"$job\"} $([[ "$code" == 0 ]] && echo 0 || echo 1)"
      echo "# HELP intermission_job_last_success_timestamp_seconds Unix time of the last successful run."
      echo "# TYPE intermission_job_last_success_timestamp_seconds gauge"
      echo "intermission_job_last_success_timestamp_seconds{task=\"$job\"} ${prev_ok:-0}"
    } > "$tmp"
    chmod 644 "$tmp"
    mv -f "$tmp" "$f"
  } || log "WARNING: could not write job status for $job"
}
