from __future__ import annotations

from datetime import datetime
from typing import Sequence

from app.domain.entities import Device, DeviceGpsConfig
from app.repositories.interfaces import DeviceRepository


class DeviceService:
    def __init__(self, devices: DeviceRepository) -> None:
        self._devices = devices

    async def list_devices(self) -> Sequence[Device]:
        return await self._devices.list()

    async def create_device(self, device_id: str, display_name: str | None = None) -> Device:
        return await self._devices.create(device_id=device_id, display_name=display_name)

    async def update_device(
        self,
        device_id: str,
        display_name: str | None,
        temp_labels: list[str] | None = None,
        temp_addresses: list[str] | None = None,
        adc_labels: dict[str, str] | None = None,
    ) -> Device:
        return await self._devices.update(
            device_id=device_id,
            display_name=display_name,
            temp_labels=temp_labels,
            temp_addresses=temp_addresses,
            adc_labels=adc_labels,
        )

    async def touch_device(self, device_id: str, seen_at: datetime) -> Device:
        return await self._devices.touch(device_id=device_id, seen_at=seen_at)

    async def get_device(self, device_id: str) -> Device | None:
        return await self._devices.get(device_id=device_id)

    async def get_gps_config(self, device_id: str) -> DeviceGpsConfig | None:
        return await self._devices.get_gps_config(device_id=device_id)

    async def upsert_gps_config(
        self,
        device_id: str,
        has_gps: bool | None = True,
        rtcm_types: list[int] | None = None,
        mode: str | None = None,
        actual_mode: str | None = None,
    ) -> DeviceGpsConfig:
        return await self._devices.upsert_gps_config(
            device_id=device_id,
            has_gps=has_gps,
            rtcm_types=rtcm_types,
            mode=mode,
            actual_mode=actual_mode,
        )
