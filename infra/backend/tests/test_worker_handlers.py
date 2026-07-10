import json
from datetime import datetime, timezone
from unittest.mock import AsyncMock

import pytest

from app.services.devices import DeviceService
from app.services.measurements import MeasurementService
from app.worker import handle_measurement, handle_state, parse_meteo


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


def _base_measure_payload() -> dict:
    return {
        "timestampIso": "2024-03-10T12:00:00Z",
        "timestampMs": 1710072000000,
        "adc1": 1.1,
        "adc2": 2.2,
        "adc3": 3.3,
        "temps": [10.0],
        "busV": 5.0,
        "busI": 0.2,
        "busP": 1.0,
        "logUseMotor": False,
        "logDuration": 1.5,
        "logFilename": "file.csv",
    }


def test_parse_meteo_builds_reading_from_online_block():
    payload = _base_measure_payload()
    payload["meteo"] = {
        "online": True,
        "timestampMs": 1710071999000,
        "tempC": 21.4,
        "humidityPct": 55.0,
        "windSpeedMs": 3.2,
        "windDirDeg": 270,
        "pressureHpa": 1013.2,
        # rainfallMm / lightLux / uvi omitted (NaN on device) → None
    }
    reading = parse_meteo("dev1", payload)
    assert reading is not None
    assert reading.device_id == "dev1"
    assert reading.timestamp_ms == 1710071999000
    assert reading.timestamp.tzinfo is timezone.utc
    assert reading.temp_c == 21.4
    assert reading.wind_dir_deg == 270
    assert reading.rainfall_mm is None
    assert reading.uvi is None


def test_parse_meteo_none_when_offline_or_absent():
    assert parse_meteo("dev1", _base_measure_payload()) is None  # no meteo block
    offline = _base_measure_payload()
    offline["meteo"] = {"online": False}
    assert parse_meteo("dev1", offline) is None
    no_ts = _base_measure_payload()
    no_ts["meteo"] = {"online": True}  # missing timestampMs
    assert parse_meteo("dev1", no_ts) is None


@pytest.mark.parametrize("timestamp_ms", [float("nan"), float("inf"), 10**30])
def test_parse_meteo_rejects_invalid_timestamp(timestamp_ms):
    payload = _base_measure_payload()
    payload["meteo"] = {"online": True, "timestampMs": timestamp_ms}
    assert parse_meteo("dev1", payload) is None


def test_parse_meteo_converts_non_finite_fields_to_none():
    payload = _base_measure_payload()
    payload["meteo"] = {
        "online": True,
        "timestampMs": 1710071999000,
        "tempC": float("nan"),
        "pressureHpa": float("inf"),
        "windDirDeg": float("nan"),
    }
    reading = parse_meteo("dev1", payload)
    assert reading is not None
    assert reading.temp_c is None
    assert reading.pressure_hpa is None
    assert reading.wind_dir_deg is None


@pytest.mark.asyncio
async def test_handle_measurement_ignores_non_object_and_invalid_utf8():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    await handle_measurement("dev1/measure", b"[]", container)
    await handle_measurement("dev1/measure", b"\xff", container)

    devices.touch_device.assert_not_called()
    measurements.add.assert_not_called()


@pytest.mark.asyncio
async def test_handle_measurement_links_meteo_when_online():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    payload = _base_measure_payload()
    payload["meteo"] = {"online": True, "timestampMs": 1710071999000, "tempC": 21.4}

    await handle_measurement("dev1/measure", json.dumps(payload).encode("utf-8"), container)

    measurements.add.assert_awaited_once()
    _, kwargs = measurements.add.call_args
    meteo = kwargs["meteo"]
    assert meteo is not None
    assert meteo.temp_c == 21.4
    assert meteo.timestamp_ms == 1710071999000


@pytest.mark.asyncio
async def test_handle_measurement_no_meteo_when_absent():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    payload = _base_measure_payload()
    await handle_measurement("dev1/measure", json.dumps(payload).encode("utf-8"), container)

    measurements.add.assert_awaited_once()
    _, kwargs = measurements.add.call_args
    assert kwargs["meteo"] is None


@pytest.mark.asyncio
async def test_handle_state_touches_device():
    devices = AsyncMock()
    measurements = AsyncMock()
    container = FakeContainer(devices, measurements)

    await handle_state("dev9/state", container)

    devices.touch_device.assert_awaited_once()
    measurements.add.assert_not_called()
