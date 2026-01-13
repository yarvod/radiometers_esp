from __future__ import annotations

from datetime import datetime
from typing import Sequence

from app.domain.entities import Measurement
from app.repositories.interfaces import MeasurementRepository


class MeasurementService:
    def __init__(self, measurements: MeasurementRepository) -> None:
        self._measurements = measurements

    async def add(self, measurement: Measurement) -> None:
        await self._measurements.add(measurement)

    async def list(self, device_id: str, start: datetime | None, end: datetime | None, limit: int) -> Sequence[Measurement]:
        return await self._measurements.list(device_id=device_id, start=start, end=end, limit=limit)
