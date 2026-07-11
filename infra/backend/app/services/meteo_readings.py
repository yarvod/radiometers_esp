from __future__ import annotations

from datetime import datetime
from math import ceil
from typing import Sequence

from app.domain.entities import MeteoReading
from app.repositories.interfaces import MeteoReadingRepository


class MeteoReadingService:
    def __init__(self, repository: MeteoReadingRepository) -> None:
        self._repository = repository

    async def list_series(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        bucket_seconds: int | None = None,
    ) -> tuple[Sequence[MeteoReading], int, int, str, bool]:
        raw_count = await self._repository.count(device_id, start, end)
        if raw_count == 0:
            return [], 0, 0, "raw", False
        if bucket_seconds is None and raw_count <= limit:
            points = await self._repository.list(device_id, start, end, limit)
            return points, raw_count, 0, "raw", False

        min_ts, max_ts = await self._repository.bounds(device_id, start, end)
        if not min_ts or not max_ts:
            points = await self._repository.list(device_id, start, end, limit)
            return points, raw_count, 0, "raw", False
        total_seconds = max(0, int(ceil((max_ts - min_ts).total_seconds())))
        if limit <= 1:
            coverage_bucket = total_seconds + 1
        else:
            coverage_bucket = max(1, ceil(total_seconds / (limit - 1)))
        effective_bucket = max(bucket_seconds or 1, coverage_bucket)
        points = await self._repository.list_aggregated(
            device_id, start, end, effective_bucket, limit, min_ts, max_ts
        )
        return points, raw_count, effective_bucket, self._format_bucket(effective_bucket), True

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
