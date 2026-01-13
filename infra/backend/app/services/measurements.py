from __future__ import annotations

from datetime import datetime
from math import ceil
from typing import Sequence

from app.domain.entities import Measurement, MeasurementPoint
from app.repositories.interfaces import MeasurementRepository


class MeasurementService:
    def __init__(self, measurements: MeasurementRepository) -> None:
        self._measurements = measurements

    async def add(self, measurement: Measurement) -> None:
        await self._measurements.add(measurement)

    async def list(self, device_id: str, start: datetime | None, end: datetime | None, limit: int) -> Sequence[Measurement]:
        return await self._measurements.list(device_id=device_id, start=start, end=end, limit=limit)

    async def list_series(
        self, device_id: str, start: datetime | None, end: datetime | None, limit: int
    ) -> tuple[Sequence[MeasurementPoint], int, int, str, bool]:
        raw_count = await self._measurements.count(device_id=device_id, start=start, end=end)
        if raw_count == 0:
            return [], 0, 0, "raw", False
        if raw_count <= limit:
            rows = await self._measurements.list(device_id=device_id, start=start, end=end, limit=limit)
            points = [self._to_point(row) for row in rows]
            return points, raw_count, 0, "raw", False

        min_ts, max_ts = await self._measurements.bounds(device_id=device_id, start=start, end=end)
        if not min_ts or not max_ts or min_ts >= max_ts:
            rows = await self._measurements.list(device_id=device_id, start=start, end=end, limit=limit)
            points = [self._to_point(row) for row in rows]
            return points, raw_count, 0, "raw", False

        total_seconds = max(1, int((max_ts - min_ts).total_seconds()))
        bucket_seconds = max(1, ceil(total_seconds / limit))
        points = await self._measurements.list_aggregated(
            device_id=device_id,
            start=start,
            end=end,
            bucket_seconds=bucket_seconds,
            limit=limit,
        )
        return points, raw_count, bucket_seconds, self._format_bucket(bucket_seconds), True

    @staticmethod
    def _to_point(row: Measurement) -> MeasurementPoint:
        return MeasurementPoint(
            timestamp=row.timestamp,
            timestamp_ms=row.timestamp_ms,
            adc1=row.adc1,
            adc2=row.adc2,
            adc3=row.adc3,
            temps=list(row.temps or []),
            bus_v=row.bus_v,
            bus_i=row.bus_i,
            bus_p=row.bus_p,
            adc1_cal=row.adc1_cal,
            adc2_cal=row.adc2_cal,
            adc3_cal=row.adc3_cal,
        )

    @staticmethod
    def _format_bucket(seconds: int) -> str:
        if seconds < 60:
            return f"{seconds}s"
        minutes = seconds / 60
        if minutes < 60:
            value = int(minutes) if minutes.is_integer() else round(minutes, 1)
            return f"{value}m"
        hours = minutes / 60
        value = int(hours) if hours.is_integer() else round(hours, 1)
        return f"{value}h"
