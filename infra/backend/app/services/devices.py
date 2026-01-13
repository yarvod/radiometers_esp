from __future__ import annotations

from datetime import datetime
from typing import Sequence

from app.domain.entities import Device
from app.repositories.interfaces import DeviceRepository


class DeviceService:
    def __init__(self, devices: DeviceRepository) -> None:
        self._devices = devices

    async def list_devices(self) -> Sequence[Device]:
        return await self._devices.list()

    async def create_device(self, device_id: str, display_name: str | None = None) -> Device:
        return await self._devices.create(device_id=device_id, display_name=display_name)

    async def update_device(self, device_id: str, display_name: str | None) -> Device:
        return await self._devices.update(device_id=device_id, display_name=display_name)

    async def touch_device(self, device_id: str, seen_at: datetime) -> Device:
        return await self._devices.touch(device_id=device_id, seen_at=seen_at)
