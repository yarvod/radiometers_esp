from __future__ import annotations

from datetime import datetime
from typing import Sequence

from app.domain.entities import ErrorEvent
from app.repositories.interfaces import ErrorRepository


class ErrorService:
    def __init__(self, errors: ErrorRepository) -> None:
        self._errors = errors

    async def add(self, event: ErrorEvent) -> None:
        await self._errors.add(event)

    async def list(
        self, device_id: str, start: datetime | None, end: datetime | None, limit: int
    ) -> Sequence[ErrorEvent]:
        return await self._errors.list(device_id=device_id, start=start, end=end, limit=limit)
