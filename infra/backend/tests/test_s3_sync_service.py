from datetime import datetime, timezone
from unittest.mock import AsyncMock

import pytest

from app.domain.entities import DeviceS3SyncConfig, S3ObjectDescriptor
from app.services.s3_sync import S3SyncService


def sync_config() -> DeviceS3SyncConfig:
    now = datetime.now(timezone.utc)
    return DeviceS3SyncConfig(
        device_id="dev1",
        enabled=True,
        bucket="dev1",
        interval_minutes=10,
        radiometer_prefix="radiometers/",
        meteo_prefix="meteo/",
        max_files_per_prefix=10,
        last_radiometer_key=None,
        last_meteo_key=None,
        next_run_at=now,
        last_started_at=None,
        last_success_at=None,
        last_error=None,
        processed_files=0,
        inserted_measurements=0,
        inserted_meteo_readings=0,
        lease_owner=None,
        lease_until=None,
        created_at=now,
        updated_at=now,
    )


@pytest.mark.asyncio
async def test_sync_device_imports_both_prefixes_and_records_each_file():
    config = sync_config()
    repository = AsyncMock()
    repository.claim.return_value = config
    repository.list_object_states.return_value = []
    measurements = AsyncMock()
    measurements.add_many_ignore_conflicts.return_value = 1
    meteo = AsyncMock()
    meteo.add_many_ignore_conflicts.return_value = 1
    devices = AsyncMock()
    redis = AsyncMock()
    store = AsyncMock()
    now = datetime.now(timezone.utc)
    radio_object = S3ObjectDescriptor("radiometers/data_20260805_100000_1.txt", "r1", now, 100)
    meteo_object = S3ObjectDescriptor("meteo/meteo_20260805_100000_1.txt", "m1", now, 100)
    store.list_objects.side_effect = [[radio_object], [meteo_object]]
    store.get_object.side_effect = [
        (
            "timestamp_iso,timestamp_ms,adc1,adc2,adc3,temp1,bus_v,bus_i,bus_p\n"
            "2026-08-05T10:00:01Z,1,1,2,3,20,5,0.2,1\n"
        ).encode(),
        (
            "timestamp_iso,timestamp_ms,light_lux,uvi,temp_c,humidity_pct,wind_speed_ms,"
            "gust_speed_ms,wind_dir_deg,rainfall_mm,pressure_hpa\n"
            "2026-08-05T10:00:01Z,2,100,1,20,50,2,3,180,0,1000\n"
        ).encode(),
    ]
    service = S3SyncService(repository, measurements, meteo, devices, store, redis)

    summary = await service.sync_device("dev1")

    assert summary.processed_files == 2
    assert summary.inserted_measurements == 1
    assert summary.inserted_meteo_readings == 1
    measurements.add_many_ignore_conflicts.assert_awaited_once()
    imported_measurement = measurements.add_many_ignore_conflicts.await_args.args[0][0]
    assert imported_measurement.timestamp_ms is None
    meteo.add_many_ignore_conflicts.assert_awaited_once()
    imported_meteo = meteo.add_many_ignore_conflicts.await_args.args[0][0]
    assert imported_meteo.timestamp_ms is None
    devices.set_has_meteo.assert_awaited_once_with("dev1", True)
    assert repository.record_object_result.await_count == 2
    repository.finish_run.assert_awaited_once()
    assert repository.finish_run.await_args.args[-1] is None


@pytest.mark.asyncio
async def test_broken_file_is_recorded_and_does_not_block_next_file():
    config = sync_config()
    repository = AsyncMock()
    repository.claim.return_value = config
    repository.list_object_states.return_value = []
    measurements = AsyncMock()
    measurements.add_many_ignore_conflicts.return_value = 1
    meteo = AsyncMock()
    devices = AsyncMock()
    store = AsyncMock()
    redis = AsyncMock()
    now = datetime.now(timezone.utc)
    broken = S3ObjectDescriptor("radiometers/data_20260805_090000_1.txt", "bad", now, 20)
    valid = S3ObjectDescriptor("radiometers/data_20260805_100000_1.txt", "ok", now, 100)
    store.list_objects.side_effect = [[broken, valid], []]
    store.get_object.side_effect = [
        b"not,a,radiometer,csv\n1,2,3,4\n",
        (
            "timestamp_iso,adc1,adc2,adc3,bus_v,bus_i,bus_p\n"
            "2026-08-05T10:00:01Z,1,2,3,5,0.2,1\n"
        ).encode(),
    ]
    service = S3SyncService(repository, measurements, meteo, devices, store, redis)

    summary = await service.sync_device("dev1")

    assert summary.processed_files == 1
    assert summary.inserted_measurements == 1
    assert summary.errors and broken.key in summary.errors[0]
    statuses = [call.kwargs["status"] for call in repository.record_object_result.await_args_list]
    assert statuses == ["failed", "done"]
    repository.rollback.assert_awaited_once()
    assert repository.finish_run.await_args.args[-1] is not None
