from __future__ import annotations

import math
from datetime import datetime
from typing import Sequence

from app.domain.entities import MeasurementPoint, RadiometerCalibration


def _normalize_dt(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=None)
    return value.replace(tzinfo=None)


def _finite(value: float | None) -> bool:
    return value is not None and math.isfinite(value)


def _bound_temperature(
    point: MeasurementPoint,
    role: str,
    temp_addresses: Sequence[str],
    temp_bindings: dict[str, str],
) -> float | None:
    address = str(temp_bindings.get(role) or "").strip()
    if not address:
        return None
    try:
        idx = list(temp_addresses).index(address)
    except ValueError:
        return None
    if idx < 0 or idx >= len(point.temps):
        return None
    value = point.temps[idx]
    return float(value) if _finite(value) else None


def _channel_coefficients(cal: RadiometerCalibration, channel: int) -> tuple[float | None, float | None]:
    return (
        getattr(cal, f"adc{channel}_slope"),
        getattr(cal, f"adc{channel}_intercept"),
    )


def _channel_radiometer_temp(cal: RadiometerCalibration, channel: int) -> float | None:
    value = getattr(cal, f"t_adc{channel}")
    return float(value) if _finite(value) else None


def _latest_usable_calibration(
    ordered: Sequence[RadiometerCalibration],
    last_idx: int,
    channel: int,
) -> RadiometerCalibration | None:
    for idx in range(last_idx, -1, -1):
        cal = ordered[idx]
        slope, intercept = _channel_coefficients(cal, channel)
        if _finite(slope) and _finite(intercept):
            return cal
    return None


def _temperature_matched_calibration(
    ordered: Sequence[RadiometerCalibration],
    last_idx: int,
    channel: int,
    current_temp_c: float | None,
) -> RadiometerCalibration | None:
    fallback = _latest_usable_calibration(ordered, last_idx, channel)
    if current_temp_c is None:
        return fallback

    best: RadiometerCalibration | None = None
    best_score: tuple[float, int] | None = None
    for idx in range(0, last_idx + 1):
        cal = ordered[idx]
        slope, intercept = _channel_coefficients(cal, channel)
        cal_temp = _channel_radiometer_temp(cal, channel)
        if not (_finite(slope) and _finite(intercept) and _finite(cal_temp)):
            continue
        score = (abs(float(cal_temp) - current_temp_c), -idx)
        if best_score is None or score < best_score:
            best = cal
            best_score = score
    return best or fallback


def apply_brightness_temperatures(
    points: list[MeasurementPoint],
    calibrations: Sequence[RadiometerCalibration],
    temp_addresses: Sequence[str] | None = None,
    temp_bindings: dict[str, str] | None = None,
) -> None:
    if not points or not calibrations:
        return

    addresses = list(temp_addresses or [])
    bindings = dict(temp_bindings or {})
    ordered = sorted(calibrations, key=lambda item: _normalize_dt(item.created_at))
    cal_idx = 0

    for point in points:
        ts = _normalize_dt(point.timestamp)
        while cal_idx + 1 < len(ordered) and _normalize_dt(ordered[cal_idx + 1].created_at) <= ts:
            cal_idx += 1

        for channel in (1, 2, 3):
            current_temp = _bound_temperature(point, f"radiometer_adc{channel}", addresses, bindings)
            cal = _temperature_matched_calibration(ordered, cal_idx, channel, current_temp)
            if cal is None:
                continue
            slope, intercept = _channel_coefficients(cal, channel)
            if not (_finite(slope) and _finite(intercept)):
                continue
            adc_value = getattr(point, f"adc{channel}")
            setattr(point, f"brightness_temp{channel}", float(slope) * adc_value + float(intercept))
            adc_cal_value = getattr(point, f"adc{channel}_cal")
            if adc_cal_value is not None:
                setattr(point, f"cal_brightness_temp{channel}", float(slope) * adc_cal_value + float(intercept))
