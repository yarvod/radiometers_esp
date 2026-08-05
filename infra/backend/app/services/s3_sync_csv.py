from __future__ import annotations

import csv
import io
import math
import re
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import PurePosixPath
from typing import Generic, Mapping, TypeVar

from pydantic import BaseModel, ConfigDict, FiniteFloat, TypeAdapter, ValidationError, model_validator

from app.domain.entities import Measurement, MeteoReading


_FILE_1970_RE = re.compile(r"^(?:data|meteo)_1970\d{4}_\d{6}(?:_|\.)")
_FINITE_FLOAT = TypeAdapter(FiniteFloat)
T = TypeVar("T")


@dataclass
class CsvParseResult(Generic[T]):
    rows: list[T] = field(default_factory=list)
    total_rows: int = 0
    invalid_count: int = 0
    errors: list[str] = field(default_factory=list)

    @property
    def error_summary(self) -> str | None:
        if not self.errors:
            return None
        suffix = (
            ""
            if self.invalid_count <= len(self.errors)
            else f"; ещё ошибок: {self.invalid_count - len(self.errors)}"
        )
        return "; ".join(self.errors) + suffix


class _CsvModel(BaseModel):
    model_config = ConfigDict(extra="ignore")

    @model_validator(mode="before")
    @classmethod
    def normalize_blanks(cls, value: object) -> object:
        if not isinstance(value, Mapping):
            return value
        normalized: dict[object, object] = {}
        for key, item in value.items():
            if isinstance(item, str):
                item = item.strip() or None
            normalized[key] = item
        return normalized


class RadiometerCsvRow(_CsvModel):
    timestamp_iso: datetime | None = None
    adc1: FiniteFloat
    adc2: FiniteFloat
    adc3: FiniteFloat
    bus_v: FiniteFloat
    bus_i: FiniteFloat
    bus_p: FiniteFloat
    adc1_cal: FiniteFloat | None = None
    adc2_cal: FiniteFloat | None = None
    adc3_cal: FiniteFloat | None = None
    gps_lat: FiniteFloat | None = None
    gps_lon: FiniteFloat | None = None
    gps_alt: FiniteFloat | None = None
    gps_fix_quality: int | None = None
    gps_satellites: int | None = None
    gps_fix_age_ms: int | None = None


class MeteoCsvRow(_CsvModel):
    timestamp_iso: datetime | None = None
    light_lux: FiniteFloat | None = None
    uvi: FiniteFloat | None = None
    temp_c: FiniteFloat | None = None
    humidity_pct: FiniteFloat | None = None
    wind_speed_ms: FiniteFloat | None = None
    gust_speed_ms: FiniteFloat | None = None
    wind_dir_deg: int | None = None
    rainfall_mm: FiniteFloat | None = None
    pressure_hpa: FiniteFloat | None = None


def is_ignored_1970_key(object_key: str) -> bool:
    return bool(_FILE_1970_RE.match(PurePosixPath(object_key).name))


def parse_radiometer_csv(payload: bytes, device_id: str, object_key: str) -> CsvParseResult[Measurement]:
    result: CsvParseResult[Measurement] = CsvParseResult()
    reader = _reader(payload)
    headers = reader.fieldnames or []
    _require_headers(headers, {"adc1", "adc2", "adc3", "bus_v", "bus_i", "bus_p"})
    temp_columns = _temperature_columns(headers)
    log_use_motor = any(name in headers for name in ("adc1_cal", "adc2_cal", "adc3_cal"))
    filename = PurePosixPath(object_key).name

    for line_number, raw in enumerate(reader, start=2):
        result.total_rows += 1
        try:
            parsed = RadiometerCsvRow.model_validate(raw)
            timestamp = _timestamp(parsed.timestamp_iso)
            temps = [
                float(_FINITE_FLOAT.validate_python(raw[column]))
                for column in temp_columns
                if raw.get(column) not in (None, "")
            ]
            result.rows.append(
                Measurement(
                    id=str(uuid.uuid4()),
                    device_id=device_id,
                    timestamp=timestamp,
                    timestamp_ms=None,
                    adc1=float(parsed.adc1),
                    adc2=float(parsed.adc2),
                    adc3=float(parsed.adc3),
                    temps=temps,
                    bus_v=float(parsed.bus_v),
                    bus_i=float(parsed.bus_i),
                    bus_p=float(parsed.bus_p),
                    adc1_cal=_float_or_none(parsed.adc1_cal),
                    adc2_cal=_float_or_none(parsed.adc2_cal),
                    adc3_cal=_float_or_none(parsed.adc3_cal),
                    gps_lat=_float_or_none(parsed.gps_lat),
                    gps_lon=_float_or_none(parsed.gps_lon),
                    gps_alt=_float_or_none(parsed.gps_alt),
                    gps_fix_quality=parsed.gps_fix_quality,
                    gps_satellites=parsed.gps_satellites,
                    gps_fix_age_ms=parsed.gps_fix_age_ms,
                    log_use_motor=log_use_motor,
                    log_duration=1.0,
                    log_filename=filename,
                )
            )
        except (ValidationError, ValueError, TypeError) as exc:
            _record_error(result, line_number, exc)
    return result


def parse_meteo_csv(payload: bytes, device_id: str) -> CsvParseResult[MeteoReading]:
    result: CsvParseResult[MeteoReading] = CsvParseResult()
    reader = _reader(payload)
    headers = reader.fieldnames or []
    _require_headers(headers, set())

    for line_number, raw in enumerate(reader, start=2):
        result.total_rows += 1
        try:
            parsed = MeteoCsvRow.model_validate(raw)
            timestamp = _timestamp(parsed.timestamp_iso)
            result.rows.append(
                MeteoReading(
                    device_id=device_id,
                    timestamp=timestamp,
                    timestamp_ms=None,
                    temp_c=_float_or_none(parsed.temp_c),
                    humidity_pct=_float_or_none(parsed.humidity_pct),
                    wind_speed_ms=_float_or_none(parsed.wind_speed_ms),
                    gust_speed_ms=_float_or_none(parsed.gust_speed_ms),
                    wind_dir_deg=parsed.wind_dir_deg,
                    pressure_hpa=_float_or_none(parsed.pressure_hpa),
                    rainfall_mm=_float_or_none(parsed.rainfall_mm),
                    light_lux=_float_or_none(parsed.light_lux),
                    uvi=_float_or_none(parsed.uvi),
                )
            )
        except (ValidationError, ValueError, TypeError) as exc:
            _record_error(result, line_number, exc)
    return result


def _reader(payload: bytes) -> csv.DictReader:
    try:
        text = payload.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError("CSV is not valid UTF-8") from exc
    reader = csv.DictReader(io.StringIO(text, newline=""))
    if reader.fieldnames is None:
        raise ValueError("CSV header is missing")
    reader.fieldnames = [name.strip() for name in reader.fieldnames]
    return reader


def _require_headers(headers: list[str], required: set[str]) -> None:
    missing = sorted(required.difference(headers))
    if missing:
        raise ValueError(f"CSV header misses required fields: {', '.join(missing)}")
    if "timestamp_iso" not in headers:
        raise ValueError("CSV header has no timestamp_iso")


def _temperature_columns(headers: list[str]) -> list[str]:
    try:
        start = headers.index("adc3") + 1
        end = headers.index("bus_v")
    except ValueError:
        return []
    return headers[start:end]


def _timestamp(timestamp_iso: datetime | None) -> datetime:
    if timestamp_iso is None:
        raise ValueError("timestamp_iso is absent")
    if timestamp_iso.tzinfo is None:
        timestamp_iso = timestamp_iso.replace(tzinfo=timezone.utc)
    timestamp = timestamp_iso.astimezone(timezone.utc)
    if not 2000 <= timestamp.year < 2100:
        raise ValueError("timestamp_iso is outside the supported 2000..2099 range")
    return timestamp


def _float_or_none(value: float | None) -> float | None:
    if value is None:
        return None
    parsed = float(value)
    return parsed if math.isfinite(parsed) else None


def _record_error(result: CsvParseResult, line_number: int, exc: Exception) -> None:
    result.invalid_count += 1
    if len(result.errors) >= 5:
        return
    if isinstance(exc, ValidationError):
        details = exc.errors(include_url=False)
        message = ", ".join(
            f"{'.'.join(str(part) for part in item['loc'])}: {item['msg']}" for item in details[:3]
        )
    else:
        message = str(exc)
    result.errors.append(f"строка {line_number}: {message}")
