from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from datetime import datetime, tzinfo
from math import ceil
from typing import Sequence

from app.domain.entities import Measurement, MeasurementPoint, MeteoReading
from app.repositories.interfaces import MeasurementRepository, MeteoReadingRepository
from app.services.temp_outliers import (
    TemperatureOutlierFilterConfig,
    TemperatureOutlierFilterStats,
    bound_temperature_indices,
    find_temperature_outlier_rows,
    filter_temperature_outliers,
    normalize_filter_config,
)

_STREAM_BATCH_SIZE = 20_000
_AVERAGE_FLOAT_FIELDS = (
    "adc1",
    "adc2",
    "adc3",
    "bus_v",
    "bus_i",
    "bus_p",
    "adc1_cal",
    "adc2_cal",
    "adc3_cal",
    "gps_lat",
    "gps_lon",
    "gps_alt",
    "gps_fix_age_ms",
)
_MAX_INT_FIELDS = ("gps_fix_quality", "gps_satellites")


@dataclass
class _BucketAccumulator:
    sums: dict[str, float] = field(default_factory=dict)
    counts: dict[str, int] = field(default_factory=dict)
    maxima: dict[str, int] = field(default_factory=dict)
    temp_sums: list[float] = field(default_factory=list)
    temp_counts: list[int] = field(default_factory=list)
    timezone: tzinfo | None = None

    def add(self, point: MeasurementPoint) -> None:
        if self.timezone is None:
            self.timezone = point.timestamp.tzinfo
        for attr in _AVERAGE_FLOAT_FIELDS:
            value = getattr(point, attr)
            if value is None:
                continue
            self.sums[attr] = self.sums.get(attr, 0.0) + float(value)
            self.counts[attr] = self.counts.get(attr, 0) + 1
        for attr in _MAX_INT_FIELDS:
            value = getattr(point, attr)
            if value is None:
                continue
            parsed = int(value)
            current = self.maxima.get(attr)
            self.maxima[attr] = parsed if current is None else max(current, parsed)
        while len(self.temp_sums) < len(point.temps):
            self.temp_sums.append(0.0)
            self.temp_counts.append(0)
        for idx, value in enumerate(point.temps):
            self.temp_sums[idx] += float(value)
            self.temp_counts[idx] += 1

    def average(self, attr: str, default: float | None = None) -> float | None:
        count = self.counts.get(attr, 0)
        return self.sums[attr] / count if count else default

    def to_point(self, bucket_epoch: int) -> MeasurementPoint:
        timestamp = datetime.fromtimestamp(bucket_epoch, tz=self.timezone)
        return MeasurementPoint(
            timestamp=timestamp,
            timestamp_ms=int(timestamp.timestamp() * 1000),
            adc1=float(self.average("adc1", 0.0) or 0.0),
            adc2=float(self.average("adc2", 0.0) or 0.0),
            adc3=float(self.average("adc3", 0.0) or 0.0),
            temps=[
                self.temp_sums[idx] / count
                for idx, count in enumerate(self.temp_counts)
                if count
            ],
            bus_v=float(self.average("bus_v", 0.0) or 0.0),
            bus_i=float(self.average("bus_i", 0.0) or 0.0),
            bus_p=float(self.average("bus_p", 0.0) or 0.0),
            adc1_cal=self.average("adc1_cal"),
            adc2_cal=self.average("adc2_cal"),
            adc3_cal=self.average("adc3_cal"),
            gps_lat=self.average("gps_lat"),
            gps_lon=self.average("gps_lon"),
            gps_alt=self.average("gps_alt"),
            gps_fix_quality=self.maxima.get("gps_fix_quality"),
            gps_satellites=self.maxima.get("gps_satellites"),
            gps_fix_age_ms=(
                int(value)
                if (value := self.average("gps_fix_age_ms")) is not None
                else None
            ),
        )


class MeasurementService:
    def __init__(
        self,
        measurements: MeasurementRepository,
        meteo_readings: MeteoReadingRepository,
    ) -> None:
        self._measurements = measurements
        self._meteo_readings = meteo_readings

    async def add(self, measurement: Measurement, meteo: MeteoReading | None = None) -> None:
        if meteo is not None:
            measurement.meteo_reading_id = await self._meteo_readings.upsert(meteo)
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

        if raw_count > limit:
            min_ts, max_ts = await self._measurements.bounds(device_id=device_id, start=start, end=end)
            if not min_ts or not max_ts:
                _, stats = filter_temperature_outliers(
                    [],
                    config,
                    temp_addresses=temp_addresses,
                    temp_bindings=temp_bindings,
                )
                return [], raw_count, 0, "raw", False, stats
            if bucket_seconds is None or bucket_seconds <= 0:
                total_seconds = max(1, int((max_ts - min_ts).total_seconds()))
                bucket_seconds = max(1, ceil(total_seconds / limit))
            points, stats = await self._stream_filter_and_aggregate(
                device_id=device_id,
                start=start,
                end=end,
                raw_count=raw_count,
                bucket_seconds=bucket_seconds,
                limit=limit,
                config=config,
                temp_addresses=temp_addresses,
                temp_bindings=temp_bindings,
            )
            return points, raw_count, bucket_seconds, self._format_bucket(bucket_seconds), True, stats

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

    async def _stream_filter_and_aggregate(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        raw_count: int,
        bucket_seconds: int,
        limit: int,
        config: TemperatureOutlierFilterConfig,
        temp_addresses: Sequence[str] | None,
        temp_bindings: dict[str, str] | None,
    ) -> tuple[list[MeasurementPoint], TemperatureOutlierFilterStats]:
        radius = config.window // 2
        left_context: list[MeasurementPoint] = []
        pending: list[MeasurementPoint] = []
        buckets: dict[int, _BucketAccumulator] = {}
        inspected_indices: list[int] | None = None
        removed_count = 0

        async def process_core(core_size: int) -> None:
            nonlocal left_context, pending, inspected_indices, removed_count
            if core_size <= 0:
                return
            core = pending[:core_size]
            right_context = pending[core_size : core_size + radius]
            window_points = [*left_context, *core, *right_context]
            if inspected_indices is None:
                inspected_indices = bound_temperature_indices(
                    window_points,
                    temp_addresses=temp_addresses,
                    temp_bindings=temp_bindings,
                )
            outlier_rows, _ = await asyncio.to_thread(
                find_temperature_outlier_rows,
                window_points,
                config,
                temp_addresses,
                temp_bindings,
                inspected_indices,
            )
            offset = len(left_context)
            for idx, point in enumerate(core):
                if offset + idx in outlier_rows:
                    removed_count += 1
                    continue
                bucket_epoch = int(point.timestamp.timestamp() // bucket_seconds) * bucket_seconds
                accumulator = buckets.get(bucket_epoch)
                if accumulator is None:
                    if len(buckets) >= limit:
                        continue
                    accumulator = _BucketAccumulator()
                    buckets[bucket_epoch] = accumulator
                accumulator.add(point)
            left_context = core[-radius:] if radius else []
            pending = pending[core_size:]
            await asyncio.sleep(0)

        async for batch in self._measurements.stream_points(
            device_id=device_id,
            start=start,
            end=end,
            batch_size=_STREAM_BATCH_SIZE,
        ):
            pending.extend(batch)
            while len(pending) >= _STREAM_BATCH_SIZE + radius:
                await process_core(_STREAM_BATCH_SIZE)
        await process_core(len(pending))

        points = [
            buckets[bucket_epoch].to_point(bucket_epoch)
            for bucket_epoch in sorted(buckets)[:limit]
        ]
        stats = TemperatureOutlierFilterStats(
            enabled=True,
            window=config.window,
            threshold=config.threshold,
            min_count=config.min_count,
            inspected_indices=inspected_indices or [],
            removed_count=removed_count,
            input_count=raw_count,
            output_count=max(0, raw_count - removed_count),
        )
        return points, stats

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
