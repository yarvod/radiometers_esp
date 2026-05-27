#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INFRA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

COMPOSE_FILE="${COMPOSE_FILE:-compose-prod.yml}"
ENV_FILE="${ENV_FILE:-$INFRA_DIR/.env}"
BACKUP_ENV_FILE="${BACKUP_ENV_FILE:-$INFRA_DIR/backup.env}"
BACKUP_DOCKER_NETWORK="${BACKUP_DOCKER_NETWORK:-}"
MINIO_MC_IMAGE="${MINIO_MC_IMAGE:-quay.io/minio/mc:latest}"
MINIO_INTERNAL_URL="${MINIO_INTERNAL_URL:-http://minio:9000}"
RESTORE_STOP_SERVICES="${RESTORE_STOP_SERVICES:-1}"

restore_postgres=1
restore_s3=1
archive_path=""

log() {
  printf '[%s] %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"
}

usage() {
  cat <<'EOF'
Usage:
  infra/scripts/restore.sh ARCHIVE [--postgres-only|--s3-only] [--no-stop]

Examples:
  infra/scripts/restore.sh infra/backups/radiometer-backup-20260527T120000Z.tar.zst
  infra/scripts/restore.sh ./backup.tar.gz --postgres-only
EOF
}

load_env_file() {
  local file="$1"
  local line key value
  if [ -f "$file" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      line="${line#"${line%%[![:space:]]*}"}"
      case "$line" in
        "" | \#*) continue ;;
      esac
      [[ "$line" == *=* ]] || continue
      line="${line#export }"
      key="${line%%=*}"
      value="${line#*=}"
      key="$(printf '%s' "$key" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
      value="$(printf '%s' "$value" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
      if [[ "$value" == \"*\" && "$value" == *\" ]]; then
        value="${value:1:${#value}-2}"
      elif [[ "$value" == \'* && "$value" == *\' ]]; then
        value="${value:1:${#value}-2}"
      fi
      if [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        export "$key=$value"
      fi
    done <"$file"
  fi
}

require_command() {
  local name="$1"
  if ! command -v "$name" >/dev/null 2>&1; then
    printf 'Missing required command: %s\n' "$name" >&2
    exit 1
  fi
}

compose() {
  if [ -f "$ENV_FILE" ]; then
    docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"
  else
    docker compose -f "$COMPOSE_FILE" "$@"
  fi
}

extract_archive() {
  local archive="$1"
  local dest="$2"

  case "$archive" in
    *.tar.zst)
      require_command zstd
      zstd -dc "$archive" | tar -C "$dest" -xf -
      ;;
    *.tar.gz | *.tgz)
      tar -C "$dest" -xzf "$archive"
      ;;
    *)
      printf 'Unsupported archive format: %s\n' "$archive" >&2
      exit 1
      ;;
  esac
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --postgres-only)
      restore_postgres=1
      restore_s3=0
      ;;
    --s3-only)
      restore_postgres=0
      restore_s3=1
      ;;
    --no-stop)
      RESTORE_STOP_SERVICES=0
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    -*)
      printf 'Unknown option: %s\n' "$1" >&2
      usage
      exit 1
      ;;
    *)
      if [ -n "$archive_path" ]; then
        printf 'Only one archive path is allowed\n' >&2
        exit 1
      fi
      archive_path="$1"
      ;;
  esac
  shift
done

if [ -z "$archive_path" ]; then
  usage
  exit 1
fi

if [ ! -f "$archive_path" ]; then
  printf 'Archive not found: %s\n' "$archive_path" >&2
  exit 1
fi
archive_path="$(cd "$(dirname "$archive_path")" && pwd)/$(basename "$archive_path")"

load_env_file "$ENV_FILE"
load_env_file "$BACKUP_ENV_FILE"

require_command docker
require_command tar

cd "$INFRA_DIR"

if [ -z "$BACKUP_DOCKER_NETWORK" ]; then
  BACKUP_DOCKER_NETWORK="${COMPOSE_PROJECT_NAME:-$(basename "$INFRA_DIR")}_default"
fi

restore_dir="$(mktemp -d "${TMPDIR:-/tmp}/radiometer-restore.XXXXXX")"

cleanup() {
  rm -rf "$restore_dir"
}
trap cleanup EXIT

log "Extracting $archive_path"
extract_archive "$archive_path" "$restore_dir"

compose up -d db minio >/dev/null

if [ "$RESTORE_STOP_SERVICES" = "1" ]; then
  log "Stopping app workers before restore"
  compose stop backend worker arq-worker >/dev/null || true
fi

if [ "$restore_postgres" = "1" ]; then
  if [ ! -f "$restore_dir/postgres/database.dump" ]; then
    printf 'Postgres dump not found in archive\n' >&2
    exit 1
  fi

  log "Restoring Postgres database ${POSTGRES_DB:-radiometer}"
  compose exec -T db sh -c 'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -v ON_ERROR_STOP=1' <<'SQL'
DROP SCHEMA IF EXISTS public CASCADE;
CREATE SCHEMA public;
GRANT ALL ON SCHEMA public TO public;
SQL
  compose exec -T db sh -c 'pg_restore -U "$POSTGRES_USER" -d "$POSTGRES_DB" --no-owner --no-acl' <"$restore_dir/postgres/database.dump"
fi

if [ "$restore_s3" = "1" ]; then
  if [ ! -d "$restore_dir/s3/buckets" ]; then
    printf 'S3 bucket dump not found in archive\n' >&2
    exit 1
  fi

  log "Restoring MinIO buckets through S3 API"
  docker run --rm \
    --network "$BACKUP_DOCKER_NETWORK" \
    -e MINIO_ENDPOINT="${MINIO_INTERNAL_URL%/}" \
    -e MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}" \
    -e MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}" \
    -v "$restore_dir/s3:/restore:ro" \
    "$MINIO_MC_IMAGE" \
    sh -eu -c '
      mc alias set radiometer "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
      if [ -f /restore/buckets.txt ]; then
        bucket_file=/restore/buckets.txt
      else
        find /restore/buckets -mindepth 1 -maxdepth 1 -type d -exec basename {} \; > /tmp/buckets.txt
        bucket_file=/tmp/buckets.txt
      fi
      while IFS= read -r bucket; do
        [ -n "$bucket" ] || continue
        mc mb --ignore-existing "radiometer/$bucket" >/dev/null
        mc mirror --overwrite --remove --preserve "/restore/buckets/$bucket" "radiometer/$bucket"
      done < "$bucket_file"
    '
fi

if [ "$RESTORE_STOP_SERVICES" = "1" ]; then
  log "Starting app services"
  compose up -d backend worker arq-worker >/dev/null
fi

log "Restore done"
