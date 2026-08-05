from __future__ import annotations

import hashlib
from datetime import datetime, timezone
from math import isfinite

from app.domain.entities import Measurement, MeteoReading


def measurement_recovery_hash(measurement: Measurement) -> str:
    """Identity shared by a firmware CSV row and its MQTT publication.

    The firmware writes the required radiometer values to CSV with less
    precision than cJSON uses for MQTT.  Canonicalizing to those CSV
    precisions lets both representations of the same sample compare equal,
    while retaining multiple real samples that happen within one ISO second.
    timestamp_ms is deliberately excluded.
    """

    values = [
        _timestamp(measurement.timestamp),
        _float(measurement.adc1, 6),
        _float(measurement.adc2, 6),
        _float(measurement.adc3, 6),
        ",".join(_float(value, 2) for value in measurement.temps),
        _float(measurement.bus_v, 3),
        _float(measurement.bus_i, 3),
        _float(measurement.bus_p, 3),
    ]
    return _digest(values)


def meteo_recovery_hash(reading: MeteoReading) -> str:
    """Identity shared by WN90LP CSV and MQTT representations."""

    values = [
        _timestamp(reading.timestamp),
        _optional_float(reading.light_lux, 1),
        _optional_float(reading.uvi, 1),
        _optional_float(reading.temp_c, 1),
        _optional_float(reading.humidity_pct, 0),
        _optional_float(reading.wind_speed_ms, 1),
        _optional_float(reading.gust_speed_ms, 1),
        str(reading.wind_dir_deg if reading.wind_dir_deg is not None else -1),
        _optional_float(reading.rainfall_mm, 1),
        _optional_float(reading.pressure_hpa, 1),
    ]
    return _digest(values)


def _timestamp(value: datetime) -> str:
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc).replace(microsecond=0).isoformat()


def _optional_float(value: float | None, precision: int) -> str:
    # The WN90LP CSV writer serializes unavailable float fields as zero.
    return _float(0.0 if value is None else value, precision)


def _float(value: float, precision: int) -> str:
    parsed = float(value)
    if not isfinite(parsed):
        return str(parsed)
    return f"{parsed:.{precision}f}"


def _digest(values: list[str]) -> str:
    return hashlib.sha256("\x1f".join(values).encode("utf-8")).hexdigest()
