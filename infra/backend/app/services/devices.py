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
        temp_label_map: dict[str, str] | None = None,
        adc_labels: dict[str, str] | None = None,
    ) -> Device:
        if temp_label_map is not None:
            temp_label_map = {
                str(address).strip(): str(label).strip()
                for address, label in temp_label_map.items()
                if str(address).strip() and str(label).strip()
            }
        elif temp_addresses is not None and temp_labels is not None:
            temp_label_map = {
                address: label
                for address, label in zip(temp_addresses, temp_labels)
                if address and label
            }

        if temp_addresses is not None and temp_labels is None:
            existing = await self._devices.get(device_id=device_id)
            if existing:
                labels_by_address = {
                    address: label
                    for address, label in zip(existing.temp_addresses, existing.temp_labels)
                    if address and label
                }
                labels_by_address.update(existing.temp_label_map or {})
                labels_by_address.update(temp_label_map or {})
                had_addresses = any(existing.temp_addresses)
                aligned_labels: list[str] = []
                for idx, address in enumerate(temp_addresses):
                    if address and address in labels_by_address:
                        aligned_labels.append(labels_by_address[address])
                    elif not had_addresses and idx < len(existing.temp_labels) and existing.temp_labels[idx]:
                        aligned_labels.append(existing.temp_labels[idx])
                    else:
                        aligned_labels.append(f"t{idx + 1}")
                temp_labels = aligned_labels
                if temp_label_map is None:
                    temp_label_map = dict(existing.temp_label_map or {})
                    temp_label_map.update(
                        {
                            address: label
                            for address, label in zip(temp_addresses, temp_labels)
                            if address and label
                        }
                    )
            else:
                temp_labels = [
                    ((temp_label_map or {}).get(address) if address else None) or f"t{idx + 1}"
                    for idx, address in enumerate(temp_addresses)
                ]
                if temp_label_map is None:
                    temp_label_map = {
                        address: label
                        for address, label in zip(temp_addresses, temp_labels)
                        if address and label
                    }
        return await self._devices.update(
            device_id=device_id,
            display_name=display_name,
            temp_labels=temp_labels,
            temp_addresses=temp_addresses,
            temp_label_map=temp_label_map,
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
