#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INFRA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

COMPOSE_FILE="${COMPOSE_FILE:-compose-prod.yml}"
ENV_FILE="${ENV_FILE:-$INFRA_DIR/.env}"
BACKUP_ENV_FILE="${BACKUP_ENV_FILE:-$INFRA_DIR/backup.env}"
BACKUP_DIR="${BACKUP_DIR:-$INFRA_DIR/backups}"
BACKUP_UPLOAD="${BACKUP_UPLOAD:-auto}"
BACKUP_RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-14}"
BACKUP_RCLONE_REMOTE="${BACKUP_RCLONE_REMOTE:-}"
BACKUP_DOCKER_NETWORK="${BACKUP_DOCKER_NETWORK:-}"
MINIO_MC_IMAGE="${MINIO_MC_IMAGE:-quay.io/minio/mc:latest}"
MINIO_INTERNAL_URL="${MINIO_INTERNAL_URL:-http://minio:9000}"
S3_BACKUP_BUCKETS="${S3_BACKUP_BUCKETS:-}"

log() {
  printf '[%s] %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"
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

compress_dir() {
  local source_dir="$1"
  local archive_base="$2"

  if command -v zstd >/dev/null 2>&1; then
    tar -C "$source_dir" -cf - . | zstd -T0 -10 -o "${archive_base}.tar.zst" >/dev/null
    printf '%s.tar.zst\n' "$archive_base"
  else
    tar -C "$source_dir" -czf "${archive_base}.tar.gz" .
    printf '%s.tar.gz\n' "$archive_base"
  fi
}

load_env_file "$ENV_FILE"
load_env_file "$BACKUP_ENV_FILE"

require_command docker
require_command tar

cd "$INFRA_DIR"
mkdir -p "$BACKUP_DIR"

if [ -z "$BACKUP_DOCKER_NETWORK" ]; then
  BACKUP_DOCKER_NETWORK="${COMPOSE_PROJECT_NAME:-$(basename "$INFRA_DIR")}_default"
fi

timestamp="$(date -u '+%Y%m%dT%H%M%SZ')"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/radiometer-backup.XXXXXX")"
archive_base="$BACKUP_DIR/radiometer-backup-$timestamp"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

log "Starting backup in $work_dir"
compose up -d db minio >/dev/null

mkdir -p "$work_dir/postgres" "$work_dir/s3/buckets"

log "Dumping Postgres"
compose exec -T db sh -c 'pg_dumpall -U "$POSTGRES_USER" --globals-only' >"$work_dir/postgres/globals.sql"
compose exec -T db sh -c 'pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --format=custom --blobs --no-owner --no-acl' >"$work_dir/postgres/database.dump"

log "Mirroring MinIO buckets through S3 API"
docker run --rm \
  --network "$BACKUP_DOCKER_NETWORK" \
  -e MINIO_ENDPOINT="${MINIO_INTERNAL_URL%/}" \
  -e MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}" \
  -e MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}" \
  -e S3_BACKUP_BUCKETS="$S3_BACKUP_BUCKETS" \
  -v "$work_dir/s3:/backup" \
  "$MINIO_MC_IMAGE" \
  sh -eu -c '
    mc alias set radiometer "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
    if [ -n "${S3_BACKUP_BUCKETS:-}" ]; then
      printf "%s\n" $S3_BACKUP_BUCKETS > /backup/buckets.txt
    else
      mc ls radiometer | awk "{print \$NF}" | sed "s#/\$##" > /backup/buckets.txt
    fi
    while IFS= read -r bucket; do
      [ -n "$bucket" ] || continue
      mkdir -p "/backup/buckets/$bucket"
      mc mirror --overwrite --preserve "radiometer/$bucket" "/backup/buckets/$bucket"
    done < /backup/buckets.txt
  '

cat >"$work_dir/manifest.txt" <<EOF
created_utc=$timestamp
compose_file=$COMPOSE_FILE
compose_project=${COMPOSE_PROJECT_NAME:-$(basename "$INFRA_DIR")}
postgres_db=${POSTGRES_DB:-radiometer}
minio_internal_url=$MINIO_INTERNAL_URL
EOF

log "Creating archive"
archive_path="$(compress_dir "$work_dir" "$archive_base")"
sha256sum "$archive_path" >"${archive_path}.sha256" 2>/dev/null || shasum -a 256 "$archive_path" >"${archive_path}.sha256"
log "Archive created: $archive_path"

if [ -n "$BACKUP_RCLONE_REMOTE" ] && { [ "$BACKUP_UPLOAD" = "auto" ] || [ "$BACKUP_UPLOAD" = "1" ]; }; then
  require_command rclone
  log "Uploading archive to $BACKUP_RCLONE_REMOTE"
  rclone copy "$archive_path" "$BACKUP_RCLONE_REMOTE" ${RCLONE_FLAGS:-}
  rclone copy "${archive_path}.sha256" "$BACKUP_RCLONE_REMOTE" ${RCLONE_FLAGS:-}
  log "Upload finished"
elif [ "$BACKUP_UPLOAD" = "1" ]; then
  printf 'BACKUP_UPLOAD=1 requires BACKUP_RCLONE_REMOTE in %s\n' "$BACKUP_ENV_FILE" >&2
  exit 1
else
  log "Upload skipped: BACKUP_RCLONE_REMOTE is not configured"
fi

if [ "$BACKUP_RETENTION_DAYS" -gt 0 ] 2>/dev/null; then
  find "$BACKUP_DIR" -type f \( -name 'radiometer-backup-*.tar.gz' -o -name 'radiometer-backup-*.tar.zst' -o -name 'radiometer-backup-*.sha256' \) -mtime +"$BACKUP_RETENTION_DAYS" -delete
fi

log "Backup done"
