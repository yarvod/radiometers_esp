import json
from datetime import datetime, timezone
from unittest.mock import AsyncMock

import pytest

from app.services.devices import DeviceService
from app.services.measurements import MeasurementService
from app.worker import handle_measurement, handle_state


class FakeContainer:
    def __init__(self, devices: DeviceService, measurements: MeasurementService) -> None:
        self._devices = devices
        self._measurements = measurements

    def __call__(self):
        return self

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        return False

    async def get(self, service_type):
        if service_type is DeviceService:
            return self._devices
        if service_type is MeasurementService:
            return self._measurements
        raise KeyError(service_type)


@pytest.mark.asyncio
async def test_handle_measurement_persists_and_touches():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    payload = {
        "timestampIso": "2024-03-10T12:00:00Z",
        "timestampMs": 1710072000000,
        "adc1": 1.1,
        "adc2": 2.2,
        "adc3": 3.3,
        "temps": [10.0, 11.1],
        "busV": 5.0,
        "busI": 0.2,
        "busP": 1.0,
        "logUseMotor": False,
        "logDuration": 1.5,
        "logFilename": "file.csv",
    }

    await handle_measurement("dev1/measure", json.dumps(payload).encode("utf-8"), container)

    devices.touch_device.assert_awaited_once()
    measurements.add.assert_awaited_once()
    args, _ = measurements.add.call_args
    measurement = args[0]
    assert measurement.device_id == "dev1"
    assert measurement.adc1 == 1.1
    assert measurement.temps == [10.0, 11.1]
    assert measurement.log_filename == "file.csv"


@pytest.mark.asyncio
async def test_handle_state_touches_device():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    await handle_state("dev9/state", container)

    devices.touch_device.assert_awaited_once()
    measurements.add.assert_not_called()
