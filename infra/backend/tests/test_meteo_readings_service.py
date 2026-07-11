from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from sqlalchemy.dialects import postgresql
from fastapi import HTTPException

from app.api.routes.meteo_readings import parse_datetime, validate_range
from app.api.schemas import MeasurementsResponse, MeteoReadingsResponse
from app.domain.entities import MeteoReading
from app.repositories.interfaces import MeteoReadingRepository
from app.repositories.sqlalchemy import SqlMeteoReadingRepository
from app.services.meteo_readings import MeteoReadingService


def reading(second: int, direction: int | None = 350) -> MeteoReading:
    timestamp = datetime(2026, 7, 11, tzinfo=timezone.utc) + timedelta(seconds=second)
    return MeteoReading(
        device_id="dev1",
        timestamp=timestamp,
        timestamp_ms=int(timestamp.timestamp() * 1000),
        temp_c=20.0 + second,
        wind_dir_deg=direction,
    )


class FakeMeteoRepository(MeteoReadingRepository):
    def __init__(self, rows: list[MeteoReading]) -> None:
        self.rows = rows
        self.aggregated_calls: list[tuple[int, int, datetime, datetime]] = []

    async def upsert(self, reading: MeteoReading) -> str:
        return "meteo-1"

    async def count(self, device_id, start, end):
        return len(self.rows)

    async def bounds(self, device_id, start, end):
        return (self.rows[0].timestamp, self.rows[-1].timestamp) if self.rows else (None, None)

    async def list(self, device_id, start, end, limit):
        return self.rows[:limit]

    async def list_aggregated(self, device_id, start, end, bucket_seconds, limit, origin, coverage_end):
        self.aggregated_calls.append((bucket_seconds, limit, origin, coverage_end))
        return self.rows[:limit]


@pytest.mark.asyncio
async def test_meteo_series_empty_range():
    service = MeteoReadingService(FakeMeteoRepository([]))
    assert await service.list_series("dev1", None, None, 2000) == ([], 0, 0, "raw", False)


@pytest.mark.asyncio
async def test_meteo_series_returns_bounded_raw_rows():
    repo = FakeMeteoRepository([reading(0), reading(9)])
    service = MeteoReadingService(repo)
    points, raw_count, bucket_seconds, label, aggregated = await service.list_series("dev1", None, None, 2000)
    assert list(points) == repo.rows
    assert (raw_count, bucket_seconds, label, aggregated) == (2, 0, "raw", False)


@pytest.mark.asyncio
async def test_meteo_series_auto_and_manual_buckets():
    rows = [reading(second) for second in range(0, 100, 10)]
    repo = FakeMeteoRepository(rows)
    service = MeteoReadingService(repo)

    _, raw_count, bucket_seconds, _, aggregated = await service.list_series("dev1", None, None, limit=3)
    assert raw_count == 10
    assert aggregated is True
    assert bucket_seconds == 45
    assert repo.aggregated_calls[-1] == (45, 3, rows[0].timestamp, rows[-1].timestamp)

    _, _, bucket_seconds, label, aggregated = await service.list_series(
        "dev1", None, None, limit=3, bucket_seconds=60
    )
    assert (bucket_seconds, label, aggregated) == (60, "1m", True)
    assert repo.aggregated_calls[-1] == (60, 3, rows[0].timestamp, rows[-1].timestamp)


@pytest.mark.asyncio
async def test_meteo_series_expands_manual_bucket_to_cover_complete_range():
    rows = [reading(second) for second in range(0, 101, 10)]
    repo = FakeMeteoRepository(rows)
    service = MeteoReadingService(repo)

    _, _, bucket_seconds, _, _ = await service.list_series(
        "dev1", rows[0].timestamp, rows[-1].timestamp, limit=3, bucket_seconds=10
    )

    assert bucket_seconds == 50
    assert repo.aggregated_calls[-1] == (50, 3, rows[0].timestamp, rows[-1].timestamp)


@pytest.mark.asyncio
async def test_meteo_series_limit_one_uses_single_covering_bucket():
    rows = [reading(0), reading(90)]
    repo = FakeMeteoRepository(rows)
    service = MeteoReadingService(repo)

    _, _, bucket_seconds, _, _ = await service.list_series("dev1", None, None, limit=1)

    assert bucket_seconds == 91


@pytest.mark.asyncio
async def test_sql_meteo_aggregation_uses_physical_field_rules():
    session = AsyncMock()
    timestamp = datetime(2026, 7, 11, tzinfo=timezone.utc)
    session.execute.return_value = [
        SimpleNamespace(
            bucket_ts=timestamp,
            temp_c=21.0,
            humidity_pct=50.0,
            wind_speed_ms=2.0,
            gust_speed_ms=4.0,
            wind_dir_deg=0.0,
            pressure_hpa=1000.0,
            rainfall_mm=0.2,
            light_lux=100.0,
            uvi=1.0,
        )
    ]

    points = await SqlMeteoReadingRepository(session).list_aggregated(
        "dev1", None, None, 60, 2000, timestamp, timestamp
    )
    statement = session.execute.await_args.args[0]
    sql = str(statement.compile(dialect=postgresql.dialect())).lower()

    assert "avg(meteo_readings.temp_c)" in sql
    assert "max(meteo_readings.gust_speed_ms)" in sql
    assert "atan2" in sql and "sin" in sql and "cos" in sql
    assert "mod(" not in sql
    assert "floor(degrees(atan2" in sql
    assert "extract(epoch from meteo_readings.timestamp)" in sql
    assert "array_agg(meteo_readings.rainfall_mm order by meteo_readings.timestamp desc)" in sql
    assert "filter (where meteo_readings.rainfall_mm is not null)" in sql
    assert points[0].wind_dir_deg == 0


def test_measurement_response_keeps_outlier_stats_out_of_meteo_response():
    assert "temp_outlier_filter" in MeasurementsResponse.model_fields
    assert "temp_outlier_filter" not in MeteoReadingsResponse.model_fields


def test_parse_datetime_requires_valid_timezone_aware_iso_value():
    assert parse_datetime("2026-07-11T10:00:00Z") == datetime(2026, 7, 11, 10, tzinfo=timezone.utc)
    for value in ("not-a-date", "2026-07-11T10:00:00"):
        with pytest.raises(HTTPException) as error:
            parse_datetime(value)
        assert error.value.status_code == 422


def test_validate_range_requires_both_bounds_and_order():
    start = datetime(2026, 7, 11, 10, tzinfo=timezone.utc)
    end = start + timedelta(hours=1)
    assert validate_range(start, end) == (start, end)
    for bounds in ((None, end), (start, None), (end, start)):
        with pytest.raises(HTTPException) as error:
            validate_range(*bounds)
        assert error.value.status_code == 422
