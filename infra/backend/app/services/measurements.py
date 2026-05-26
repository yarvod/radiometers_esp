from __future__ import annotations

from datetime import datetime
from math import ceil
from typing import Sequence

from app.domain.entities import Measurement, MeasurementPoint
from app.repositories.interfaces import MeasurementRepository
from app.services.temp_outliers import (
    TemperatureOutlierFilterConfig,
    TemperatureOutlierFilterStats,
    filter_temperature_outliers,
    normalize_filter_config,
)


class MeasurementService:
    def __init__(self, measurements: MeasurementRepository) -> None:
        self._measurements = measurements

    async def add(self, measurement: Measurement) -> None:
        await self._measurements.add(measurement)

    async def list(self, device_id: str, start: datetime | None, end: datetime | None, limit: int) -> Sequence[Measurement]:
        return await self._measurements.list(device_id=device_id, start=start, end=end, limit=limit)

    async def list_series(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        bucket_seconds: int | None = None,
    ) -> tuple[Sequence[MeasurementPoint], int, int, str, bool]:
        raw_count = await self._measurements.count(device_id=device_id, start=start, end=end)
        if raw_count == 0:
            return [], 0, 0, "raw", False
        if bucket_seconds is not None and bucket_seconds > 0:
            points = await self._measurements.list_aggregated(
                device_id=device_id,
                start=start,
                end=end,
                bucket_seconds=bucket_seconds,
                limit=limit,
            )
            return points, raw_count, bucket_seconds, self._format_bucket(bucket_seconds), True
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

    async def list_series_with_temp_outlier_filter(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        bucket_seconds: int | None = None,
        temp_outlier_filter: TemperatureOutlierFilterConfig | None = None,
        temp_addresses: Sequence[str] | None = None,
        temp_bindings: dict[str, str] | None = None,
    ) -> tuple[Sequence[MeasurementPoint], int, int, str, bool, TemperatureOutlierFilterStats]:
        config = normalize_filter_config(temp_outlier_filter)
        if not config.enabled:
            points, raw_count, bucket_value, bucket_label, aggregated = await self.list_series(
                device_id=device_id,
                start=start,
                end=end,
                limit=limit,
                bucket_seconds=bucket_seconds,
            )
            _, stats = filter_temperature_outliers(
                points,
                config,
                temp_addresses=temp_addresses,
                temp_bindings=temp_bindings,
            )
            return points, raw_count, bucket_value, bucket_label, aggregated, stats

        raw_count = await self._measurements.count(device_id=device_id, start=start, end=end)
        if raw_count == 0:
            _, stats = filter_temperature_outliers(
                [],
                config,
                temp_addresses=temp_addresses,
                temp_bindings=temp_bindings,
            )
            return [], 0, 0, "raw", False, stats

        rows = await self._measurements.list(device_id=device_id, start=start, end=end, limit=raw_count)
        raw_points = [self._to_point(row) for row in rows]
        filtered_points, stats = filter_temperature_outliers(
            raw_points,
            config,
            temp_addresses=temp_addresses,
            temp_bindings=temp_bindings,
        )

        if bucket_seconds is not None and bucket_seconds > 0:
            points = self._aggregate_points(filtered_points, bucket_seconds, limit)
            return points, raw_count, bucket_seconds, self._format_bucket(bucket_seconds), True, stats
        if len(filtered_points) <= limit:
            return filtered_points, raw_count, 0, "raw", False, stats
        if not filtered_points:
            return [], raw_count, 0, "raw", False, stats

        min_ts = filtered_points[0].timestamp
        max_ts = filtered_points[-1].timestamp
        if min_ts >= max_ts:
            return filtered_points[:limit], raw_count, 0, "raw", False, stats

        total_seconds = max(1, int((max_ts - min_ts).total_seconds()))
        bucket_seconds = max(1, ceil(total_seconds / limit))
        points = self._aggregate_points(filtered_points, bucket_seconds, limit)
        return points, raw_count, bucket_seconds, self._format_bucket(bucket_seconds), True, stats

    async def latest_timestamp(self, device_id: str) -> datetime | None:
        _, max_ts = await self._measurements.bounds(device_id=device_id, start=None, end=None)
        return max_ts

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
            gps_lat=row.gps_lat,
            gps_lon=row.gps_lon,
            gps_alt=row.gps_alt,
            gps_fix_quality=row.gps_fix_quality,
            gps_satellites=row.gps_satellites,
            gps_fix_age_ms=row.gps_fix_age_ms,
            brightness_temp1=None,
            brightness_temp2=None,
            brightness_temp3=None,
        )

    @staticmethod
    def _aggregate_points(points: Sequence[MeasurementPoint], bucket_seconds: int, limit: int) -> list[MeasurementPoint]:
        buckets: dict[int, list[MeasurementPoint]] = {}
        for point in points:
            bucket_epoch = int(point.timestamp.timestamp() // bucket_seconds) * bucket_seconds
            buckets.setdefault(bucket_epoch, []).append(point)

        out: list[MeasurementPoint] = []
        for bucket_epoch in sorted(buckets)[:limit]:
            rows = buckets[bucket_epoch]
            first = rows[0]
            timestamp = datetime.fromtimestamp(bucket_epoch, tz=first.timestamp.tzinfo)
            max_temps = max((len(row.temps) for row in rows), default=0)
            out.append(
                MeasurementPoint(
                    timestamp=timestamp,
                    timestamp_ms=int(timestamp.timestamp() * 1000),
                    adc1=MeasurementService._avg_required(rows, "adc1"),
                    adc2=MeasurementService._avg_required(rows, "adc2"),
                    adc3=MeasurementService._avg_required(rows, "adc3"),
                    temps=[
                        value
                        for value in (
                            MeasurementService._avg_temp(rows, idx)
                            for idx in range(max_temps)
                        )
                        if value is not None
                    ],
                    bus_v=MeasurementService._avg_required(rows, "bus_v"),
                    bus_i=MeasurementService._avg_required(rows, "bus_i"),
                    bus_p=MeasurementService._avg_required(rows, "bus_p"),
                    adc1_cal=MeasurementService._avg_optional(rows, "adc1_cal"),
                    adc2_cal=MeasurementService._avg_optional(rows, "adc2_cal"),
                    adc3_cal=MeasurementService._avg_optional(rows, "adc3_cal"),
                    gps_lat=MeasurementService._avg_optional(rows, "gps_lat"),
                    gps_lon=MeasurementService._avg_optional(rows, "gps_lon"),
                    gps_alt=MeasurementService._avg_optional(rows, "gps_alt"),
                    gps_fix_quality=MeasurementService._max_optional_int(rows, "gps_fix_quality"),
                    gps_satellites=MeasurementService._max_optional_int(rows, "gps_satellites"),
                    gps_fix_age_ms=MeasurementService._avg_optional_int(rows, "gps_fix_age_ms"),
                )
            )
        return out

    @staticmethod
    def _avg_required(rows: Sequence[MeasurementPoint], attr: str) -> float:
        values = [float(getattr(row, attr)) for row in rows if getattr(row, attr) is not None]
        return sum(values) / len(values) if values else 0.0

    @staticmethod
    def _avg_optional(rows: Sequence[MeasurementPoint], attr: str) -> float | None:
        values = [float(value) for row in rows if (value := getattr(row, attr)) is not None]
        return sum(values) / len(values) if values else None

    @staticmethod
    def _avg_optional_int(rows: Sequence[MeasurementPoint], attr: str) -> int | None:
        value = MeasurementService._avg_optional(rows, attr)
        return int(value) if value is not None else None

    @staticmethod
    def _max_optional_int(rows: Sequence[MeasurementPoint], attr: str) -> int | None:
        values = [int(value) for row in rows if (value := getattr(row, attr)) is not None]
        return max(values) if values else None

    @staticmethod
    def _avg_temp(rows: Sequence[MeasurementPoint], idx: int) -> float | None:
        values = [float(row.temps[idx]) for row in rows if idx < len(row.temps)]
        return sum(values) / len(values) if values else None

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
