from __future__ import annotations

import logging
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Sequence

from arq.connections import ArqRedis

from app.clients.s3 import S3ObjectStore
from app.domain.entities import DeviceS3SyncConfig, S3ObjectDescriptor, S3SyncObjectState
from app.repositories.interfaces import (
    DeviceRepository,
    MeasurementRepository,
    MeteoReadingRepository,
    S3SyncRepository,
)
from app.services.s3_sync_csv import (
    CsvParseResult,
    is_ignored_1970_key,
    parse_meteo_csv,
    parse_radiometer_csv,
)


logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class S3SyncRunSummary:
    device_id: str
    processed_files: int
    inserted_measurements: int
    inserted_meteo_readings: int
    invalid_rows: int
    errors: tuple[str, ...]


class S3SyncService:
    def __init__(
        self,
        repository: S3SyncRepository,
        measurements: MeasurementRepository,
        meteo_readings: MeteoReadingRepository,
        devices: DeviceRepository,
        object_store: S3ObjectStore,
        redis: ArqRedis,
    ) -> None:
        self._repository = repository
        self._measurements = measurements
        self._meteo_readings = meteo_readings
        self._devices = devices
        self._object_store = object_store
        self._redis = redis

    async def get_config(self, device_id: str) -> DeviceS3SyncConfig:
        return await self._repository.get_or_create_config(device_id)

    async def update_config(
        self,
        device_id: str,
        enabled: bool | None,
        bucket: str | None,
        interval_minutes: int | None,
        radiometer_prefix: str | None,
        meteo_prefix: str | None,
        max_files_per_prefix: int | None,
    ) -> DeviceS3SyncConfig:
        normalized_bucket = None
        if bucket is not None:
            normalized_bucket = bucket.strip()
            if not normalized_bucket or "/" in normalized_bucket:
                raise ValueError("bucket must be a non-empty S3 bucket name, without slashes")
        if interval_minutes is not None and not 1 <= interval_minutes <= 10_080:
            raise ValueError("interval_minutes must be between 1 and 10080")
        if max_files_per_prefix is not None and not 1 <= max_files_per_prefix <= 100:
            raise ValueError("max_files_per_prefix must be between 1 and 100")
        return await self._repository.update_config(
            device_id=device_id,
            enabled=enabled,
            bucket=normalized_bucket,
            interval_minutes=interval_minutes,
            radiometer_prefix=_normalize_prefix(radiometer_prefix),
            meteo_prefix=_normalize_prefix(meteo_prefix),
            max_files_per_prefix=max_files_per_prefix,
        )

    async def enqueue_due(self) -> int:
        due = await self._repository.list_due(datetime.now(timezone.utc))
        enqueued = 0
        for config in due:
            job_id = f"s3-sync:{config.device_id}:{int(config.next_run_at.timestamp())}"
            job = await self._redis.enqueue_job("sync_device_s3_job", config.device_id, _job_id=job_id)
            if job is not None:
                enqueued += 1
        return enqueued

    async def enqueue_now(self, device_id: str) -> bool:
        await self._repository.get_or_create_config(device_id)
        await self._repository.commit()
        job = await self._redis.enqueue_job(
            "sync_device_s3_job",
            device_id,
            force=True,
            _job_id=f"s3-sync:{device_id}:manual:{uuid.uuid4()}",
        )
        return job is not None

    async def sync_device(self, device_id: str, force: bool = False) -> S3SyncRunSummary:
        owner = str(uuid.uuid4())
        started_at = datetime.now(timezone.utc)
        config = await self._repository.claim(
            device_id=device_id,
            owner=owner,
            now=started_at,
            lease_until=started_at + timedelta(minutes=30),
            force=force,
        )
        if config is None:
            return S3SyncRunSummary(device_id, 0, 0, 0, 0, ())
        await self._repository.commit()

        processed_files = 0
        inserted_measurements = 0
        inserted_meteo = 0
        invalid_rows = 0
        errors: list[str] = []
        try:
            states = {
                state.key: state
                for state in await self._repository.list_object_states(device_id, config.bucket)
            }
            for kind, prefix, cursor in (
                ("radiometer", config.radiometer_prefix, config.last_radiometer_key),
                ("meteo", config.meteo_prefix, config.last_meteo_key),
            ):
                try:
                    objects = await self._object_store.list_objects(config.bucket, prefix)
                except Exception as exc:
                    message = f"{kind} listing failed: {exc}"
                    errors.append(message)
                    logger.exception(
                        "S3 listing failed device=%s bucket=%s prefix=%s",
                        device_id,
                        config.bucket,
                        prefix,
                    )
                    continue

                selected = select_pending_objects(
                    objects=objects,
                    states=states,
                    cursor=cursor,
                    now=datetime.now(timezone.utc),
                    limit=config.max_files_per_prefix,
                )
                for descriptor in selected:
                    if is_ignored_1970_key(descriptor.key) or not descriptor.key.lower().endswith(".txt"):
                        await self._record_ignored(config, descriptor, kind)
                        states[descriptor.key] = S3SyncObjectState(descriptor.key, descriptor.etag, "ignored", None)
                        continue
                    try:
                        payload = await self._object_store.get_object(config.bucket, descriptor.key)
                        if kind == "radiometer":
                            parsed = parse_radiometer_csv(payload, device_id, descriptor.key)
                            if parsed.total_rows > 0 and not parsed.rows:
                                raise ValueError(parsed.error_summary or "radiometer CSV has no valid rows")
                            inserted = await self._measurements.add_many_ignore_conflicts(parsed.rows)
                        else:
                            parsed = parse_meteo_csv(payload, device_id)
                            if parsed.total_rows > 0 and not parsed.rows:
                                raise ValueError(parsed.error_summary or "meteo CSV has no valid rows")
                            inserted = await self._meteo_readings.add_many_ignore_conflicts(parsed.rows)
                            if parsed.rows:
                                await self._devices.set_has_meteo(device_id, True)
                        await self._record_done(config, descriptor, kind, parsed, inserted)
                        if kind == "radiometer":
                            inserted_measurements += inserted
                        else:
                            inserted_meteo += inserted
                        invalid_rows += parsed.invalid_count
                        states[descriptor.key] = S3SyncObjectState(descriptor.key, descriptor.etag, "done", None)
                        processed_files += 1
                        if parsed.invalid_count:
                            errors.append(f"{descriptor.key}: {parsed.error_summary}")
                    except Exception as exc:
                        await self._repository.rollback()
                        message = f"{descriptor.key}: {exc}"
                        errors.append(message)
                        logger.exception("S3 object recovery failed device=%s object=%s", device_id, descriptor.key)
                        await self._record_failed(config, descriptor, kind, message)
                        states[descriptor.key] = S3SyncObjectState(
                            descriptor.key,
                            descriptor.etag,
                            "failed",
                            datetime.now(timezone.utc) + timedelta(minutes=config.interval_minutes),
                        )

            final_error = " | ".join(errors[:10]) or None
            await self._repository.finish_run(device_id, owner, datetime.now(timezone.utc), final_error)
            await self._repository.commit()
        except Exception as exc:
            await self._repository.rollback()
            await self._repository.finish_run(device_id, owner, datetime.now(timezone.utc), str(exc))
            await self._repository.commit()
            raise

        return S3SyncRunSummary(
            device_id=device_id,
            processed_files=processed_files,
            inserted_measurements=inserted_measurements,
            inserted_meteo_readings=inserted_meteo,
            invalid_rows=invalid_rows,
            errors=tuple(errors),
        )

    async def _record_done(
        self,
        config: DeviceS3SyncConfig,
        descriptor: S3ObjectDescriptor,
        kind: str,
        parsed: CsvParseResult,
        inserted: int,
    ) -> None:
        await self._repository.record_object_result(
            device_id=config.device_id,
            bucket=config.bucket,
            object_key=descriptor.key,
            etag=descriptor.etag,
            kind=kind,
            status="done",
            row_count=parsed.total_rows,
            inserted_count=inserted,
            invalid_count=parsed.invalid_count,
            error=parsed.error_summary,
            next_retry_at=None,
            last_modified=descriptor.last_modified,
            processed_at=datetime.now(timezone.utc),
        )
        await self._repository.commit()

    async def _record_failed(
        self,
        config: DeviceS3SyncConfig,
        descriptor: S3ObjectDescriptor,
        kind: str,
        error: str,
    ) -> None:
        now = datetime.now(timezone.utc)
        await self._repository.record_object_result(
            device_id=config.device_id,
            bucket=config.bucket,
            object_key=descriptor.key,
            etag=descriptor.etag,
            kind=kind,
            status="failed",
            row_count=0,
            inserted_count=0,
            invalid_count=0,
            error=error,
            next_retry_at=now + timedelta(minutes=config.interval_minutes),
            last_modified=descriptor.last_modified,
            processed_at=now,
        )
        await self._repository.commit()

    async def _record_ignored(
        self,
        config: DeviceS3SyncConfig,
        descriptor: S3ObjectDescriptor,
        kind: str,
    ) -> None:
        reason = "filename has year 1970" if is_ignored_1970_key(descriptor.key) else "unsupported file extension"
        await self._repository.record_object_result(
            device_id=config.device_id,
            bucket=config.bucket,
            object_key=descriptor.key,
            etag=descriptor.etag,
            kind=kind,
            status="ignored",
            row_count=0,
            inserted_count=0,
            invalid_count=0,
            error=reason,
            next_retry_at=None,
            last_modified=descriptor.last_modified,
            processed_at=datetime.now(timezone.utc),
        )
        await self._repository.commit()


def select_pending_objects(
    objects: Sequence[S3ObjectDescriptor],
    states: dict[str, S3SyncObjectState],
    cursor: str | None,
    now: datetime,
    limit: int,
) -> list[S3ObjectDescriptor]:
    ordered = sorted(objects, key=lambda item: item.key)
    if cursor:
        ordered = [item for item in ordered if item.key > cursor] + [item for item in ordered if item.key <= cursor]
    selected: list[S3ObjectDescriptor] = []
    for descriptor in ordered:
        state = states.get(descriptor.key)
        if state is not None and state.etag == descriptor.etag:
            if state.status in {"done", "ignored"}:
                continue
            if state.status == "failed" and state.next_retry_at is not None and state.next_retry_at > now:
                continue
        selected.append(descriptor)
        if len(selected) >= limit:
            break
    return selected


def _normalize_prefix(value: str | None) -> str | None:
    if value is None:
        return None
    normalized = value.strip().strip("/")
    return f"{normalized}/" if normalized else ""
