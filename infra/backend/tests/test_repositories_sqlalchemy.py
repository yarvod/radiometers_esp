from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest
from sqlalchemy.dialects import postgresql

from app.domain.entities import Measurement, MeteoReading
from app.db.models import AccessTokenModel, DeviceModel, MeasurementModel
from app.repositories.sqlalchemy import (
    SqlDeviceRepository,
    SqlMeasurementRepository,
    SqlMeteoReadingRepository,
    SqlS3SyncRepository,
    SqlTokenRepository,
)


class FakeScalars:
    def __init__(self, values):
        self._values = values

    def all(self):
        return self._values


class FakeResult:
    def __init__(self, scalar=None, scalars=None, rowcount=0):
        self._scalar = scalar
        self._scalars = scalars or []
        self.rowcount = rowcount

    def scalar_one_or_none(self):
        return self._scalar

    def scalars(self):
        return FakeScalars(self._scalars)


class FakeStreamResult:
    def __init__(self, rows):
        self._rows = rows

    async def partitions(self, batch_size):
        for offset in range(0, len(self._rows), batch_size):
            yield self._rows[offset : offset + batch_size]


@pytest.mark.asyncio
async def test_s3_sync_claim_sets_updated_at_before_flush():
    now = datetime(2026, 8, 6, 12, 0, tzinfo=timezone.utc)
    lease_until = now + timedelta(minutes=30)
    model = SimpleNamespace(
        device_id="dev3",
        enabled=True,
        bucket="dev3",
        interval_minutes=10,
        radiometer_prefix="radiometers/",
        meteo_prefix="meteo/",
        max_files_per_prefix=10,
        last_radiometer_key=None,
        last_meteo_key=None,
        next_run_at=now,
        last_started_at=None,
        last_success_at=None,
        last_error="previous error",
        processed_files=0,
        inserted_measurements=0,
        inserted_meteo_readings=0,
        lease_owner=None,
        lease_until=None,
        created_at=now - timedelta(days=1),
        updated_at=now - timedelta(hours=1),
    )
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalar=model))

    async def assert_updated_at_is_set():
        assert model.updated_at == now

    session.flush = AsyncMock(side_effect=assert_updated_at_is_set)

    claimed = await SqlS3SyncRepository(session).claim(
        "dev3", "worker-1", now, lease_until
    )

    assert claimed is not None
    assert claimed.updated_at == now
    assert claimed.lease_owner == "worker-1"
    assert claimed.lease_until == lease_until
    assert claimed.next_run_at == now + timedelta(minutes=10)
    assert claimed.last_error is None
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_token_repo_create_adds_and_flushes():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()

    repo = SqlTokenRepository(session)
    expires_at = datetime.now(timezone.utc)
    token = await repo.create("token-1", "user-1", expires_at)

    assert token.token == "token-1"
    assert token.user_id == "user-1"
    assert token.expires_at == expires_at
    session.add.assert_called_once()
    assert isinstance(session.add.call_args.args[0], AccessTokenModel)
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_token_repo_get_returns_none():
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalar=None))

    repo = SqlTokenRepository(session)
    result = await repo.get("missing")

    assert result is None
    session.execute.assert_awaited_once()


@pytest.mark.asyncio
async def test_token_repo_delete_executes():
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult())

    repo = SqlTokenRepository(session)
    await repo.delete("token-1")

    session.execute.assert_awaited_once()


@pytest.mark.asyncio
async def test_device_repo_touch_updates_existing():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()

    model = DeviceModel(id="dev1", display_name=None, last_seen_at=None)
    session.execute = AsyncMock(return_value=FakeResult(scalar=model))

    repo = SqlDeviceRepository(session)
    now = datetime.now(timezone.utc)
    device = await repo.touch("dev1", now)

    assert device.last_seen_at == now
    session.add.assert_not_called()
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_device_repo_touch_creates_new():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalar=None))

    repo = SqlDeviceRepository(session)
    now = datetime.now(timezone.utc)
    device = await repo.touch("dev2", now)

    assert device.id == "dev2"
    session.add.assert_called_once()
    assert isinstance(session.add.call_args.args[0], DeviceModel)
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_measurement_repo_add_uses_recovery_hash_conflict():
    session = AsyncMock()
    session.execute = AsyncMock()

    repo = SqlMeasurementRepository(session)
    measurement = Measurement(
        id="m1",
        device_id="dev1",
        timestamp=datetime.now(timezone.utc),
        timestamp_ms=1234,
        adc1=1.0,
        adc2=2.0,
        adc3=3.0,
        temps=[10.0, 11.0],
        bus_v=5.0,
        bus_i=0.1,
        bus_p=0.5,
        adc1_cal=None,
        adc2_cal=None,
        adc3_cal=None,
        gps_lat=None,
        gps_lon=None,
        gps_alt=None,
        gps_fix_quality=None,
        gps_satellites=None,
        gps_fix_age_ms=None,
        log_use_motor=False,
        log_duration=1.0,
        log_filename=None,
    )

    await repo.add(measurement)

    session.execute.assert_awaited_once()
    statement = session.execute.await_args.args[0]
    sql = str(statement.compile(dialect=postgresql.dialect())).lower()
    assert "on conflict (device_id, recovery_hash) do nothing" in sql


@pytest.mark.asyncio
async def test_measurement_batch_keeps_distinct_rows_in_same_second():
    session = AsyncMock()
    session.execute = AsyncMock(
        side_effect=[FakeResult(scalars=[]), FakeResult(rowcount=2)]
    )
    timestamp = datetime.now(timezone.utc).replace(microsecond=0)
    common = {
        "device_id": "dev1",
        "timestamp": timestamp,
        "timestamp_ms": None,
        "adc1": 1.0,
        "adc3": 3.0,
        "temps": [10.0],
        "bus_v": 5.0,
        "bus_i": 0.1,
        "bus_p": 0.5,
        "adc1_cal": None,
        "adc2_cal": None,
        "adc3_cal": None,
        "gps_lat": None,
        "gps_lon": None,
        "gps_alt": None,
        "gps_fix_quality": None,
        "gps_satellites": None,
        "gps_fix_age_ms": None,
        "log_use_motor": False,
        "log_duration": 1.0,
        "log_filename": "data.txt",
    }
    rows = [
        Measurement(id="m1", adc2=2.0, **common),
        Measurement(id="m2", adc2=2.1, **common),
    ]

    inserted = await SqlMeasurementRepository(session).add_many_ignore_conflicts(rows)

    assert inserted == 2
    assert session.execute.await_count == 2
    insert_statement = session.execute.await_args_list[1].args[0]
    sql = str(insert_statement.compile(dialect=postgresql.dialect())).lower()
    assert "on conflict (device_id, recovery_hash) do nothing" in sql


@pytest.mark.asyncio
async def test_measurement_batch_skips_content_match_from_legacy_mqtt_row():
    timestamp = datetime.now(timezone.utc).replace(microsecond=0)
    existing = MeasurementModel(
        id="mqtt",
        device_id="dev1",
        timestamp=timestamp,
        timestamp_ms=1778067367969,
        recovery_hash=None,
        adc1=1.1234564,
        adc2=2.2345674,
        adc3=3.3456784,
        temps=[20.124],
        bus_v=5.1234,
        bus_i=0.2344,
        bus_p=1.2344,
        adc1_cal=None,
        adc2_cal=None,
        adc3_cal=None,
        gps_lat=None,
        gps_lon=None,
        gps_alt=None,
        gps_fix_quality=None,
        gps_satellites=None,
        gps_fix_age_ms=None,
        log_use_motor=False,
        log_duration=1.0,
        log_filename="data.txt",
        meteo_reading_id=None,
    )
    recovered = Measurement(
        id="s3",
        device_id="dev1",
        timestamp=timestamp,
        timestamp_ms=None,
        adc1=1.123456,
        adc2=2.234567,
        adc3=3.345678,
        temps=[20.12],
        bus_v=5.123,
        bus_i=0.234,
        bus_p=1.234,
        adc1_cal=None,
        adc2_cal=None,
        adc3_cal=None,
        gps_lat=None,
        gps_lon=None,
        gps_alt=None,
        gps_fix_quality=None,
        gps_satellites=None,
        gps_fix_age_ms=None,
        log_use_motor=False,
        log_duration=1.0,
        log_filename="data.txt",
    )
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalars=[existing]))

    inserted = await SqlMeasurementRepository(session).add_many_ignore_conflicts([recovered])

    assert inserted == 0
    session.execute.assert_awaited_once()


@pytest.mark.asyncio
async def test_measurement_repo_streams_scalar_rows_in_batches():
    timestamp = datetime.now(timezone.utc)
    rows = [
        SimpleNamespace(
            timestamp=timestamp,
            timestamp_ms=1234 + index,
            adc1=1,
            adc2=2,
            adc3=3,
            temps=[10, 11],
            bus_v=5,
            bus_i=0.1,
            bus_p=0.5,
            adc1_cal=None,
            adc2_cal=None,
            adc3_cal=None,
            gps_lat=None,
            gps_lon=None,
            gps_alt=None,
            gps_fix_quality=None,
            gps_satellites=None,
            gps_fix_age_ms=None,
        )
        for index in range(3)
    ]
    session = AsyncMock()
    session.stream = AsyncMock(return_value=FakeStreamResult(rows))

    batches = [
        batch
        async for batch in SqlMeasurementRepository(session).stream_points(
            "dev1",
            start=None,
            end=None,
            batch_size=2,
        )
    ]

    assert [len(batch) for batch in batches] == [2, 1]
    assert batches[0][0].timestamp == timestamp
    assert batches[0][0].temps == [10.0, 11.0]
    statement = session.stream.await_args.args[0]
    sql = str(statement.compile(dialect=postgresql.dialect()))
    assert "ORDER BY measurements.timestamp ASC, measurements.id ASC" in sql


@pytest.mark.asyncio
async def test_meteo_upsert_preserves_existing_non_null_fields():
    session = AsyncMock()
    result = Mock()
    result.scalar_one.return_value = "meteo-1"
    session.execute = AsyncMock(return_value=result)

    reading = MeteoReading(
        device_id="dev1",
        timestamp=datetime.now(timezone.utc),
        timestamp_ms=1710071999000,
        temp_c=21.4,
    )
    returned_id = await SqlMeteoReadingRepository(session).upsert(reading)

    stmt = session.execute.await_args.args[0]
    sql = str(stmt.compile(dialect=postgresql.dialect()))
    assert returned_id == "meteo-1"
    assert "ON CONFLICT (device_id, recovery_hash) DO UPDATE" in sql
    assert "coalesce(excluded.temp_c, meteo_readings.temp_c)" in sql
    assert "coalesce(excluded.pressure_hpa, meteo_readings.pressure_hpa)" in sql
