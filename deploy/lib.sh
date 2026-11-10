# Shared helpers for the deploy scripts. Source it; don't run it.
# shellcheck shell=bash

set -Eeuo pipefail
umask 077
# Fail loudly: name the script and line of any command that fails.
trap 'printf "%s ERROR: %s:%s: command failed (exit %s): %s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${BASH_SOURCE[0]##*/}" "$LINENO" "$?" "$BASH_COMMAND" >&2' ERR

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$DEPLOY_DIR/.." && pwd)"
ENV_FILE="${RECS_ENV_FILE:-$DEPLOY_DIR/.env}"
COMPOSE_FILE="$DEPLOY_DIR/compose.yaml"
IMAGES=(api model train web backup)

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

require_env_file() {
  [[ -f "$ENV_FILE" ]] || die "$ENV_FILE not found: copy deploy/.env.example to deploy/.env and fill it in"
  local mode
  mode="$(stat -c %a "$ENV_FILE")"
  [[ "$mode" == 600 || "$mode" == 400 ]] || log "WARNING: $ENV_FILE is mode $mode; it holds secrets, use chmod 600"
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

# short_sha REF: 7-character commit SHA for REF. Accepts a SHA that is not in the local repo
# only if its images already exist (e.g. loaded from a CI artifact).
short_sha() {
  local ref="$1" sha
  if sha="$(git -C "$REPO_DIR" rev-parse --verify --quiet "${ref}^{commit}")"; then
    printf '%s\n' "${sha:0:7}"
  elif [[ "$ref" =~ ^[0-9a-f]{7,40}$ ]]; then
    printf '%s\n' "${ref:0:7}"
  else
    die "unknown commit '$ref'"
  fi
}

images_present() {
  local tag="$1" name
  for name in "${IMAGES[@]}"; do
    docker image inspect "intermission-$name:$tag" >/dev/null 2>&1 || return 1
  done
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

# origin_host ORIGIN: host[:port] part of scheme://host[:port].
origin_host() { local o="${1#*://}"; printf '%s\n' "${o%%/*}"; }

# job_status JOB CODE: record a job result for node-exporter's textfile collector
# (alerts: NightlyJobFailed, NightlyJobStale). Never fails the caller.
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
