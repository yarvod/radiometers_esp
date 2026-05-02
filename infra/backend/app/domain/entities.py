from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Optional


@dataclass
class Device:
    id: str
    display_name: Optional[str]
    created_at: datetime
    last_seen_at: Optional[datetime]
    temp_labels: list[str]
    temp_addresses: list[str]
    adc_labels: dict[str, str]


@dataclass
class DeviceGpsConfig:
    device_id: str
    has_gps: bool
    rtcm_types: list[int]
    mode: str
    actual_mode: Optional[str]
    updated_at: Optional[datetime]
    created_at: datetime


@dataclass
class Measurement:
    id: str
    device_id: str
    timestamp: datetime
    timestamp_ms: Optional[int]
    adc1: float
    adc2: float
    adc3: float
    temps: list[float]
    bus_v: float
    bus_i: float
    bus_p: float
    adc1_cal: Optional[float]
    adc2_cal: Optional[float]
    adc3_cal: Optional[float]
    log_use_motor: bool
    log_duration: float
    log_filename: Optional[str]


@dataclass
class MeasurementPoint:
    timestamp: datetime
    timestamp_ms: Optional[int]
    adc1: float
    adc2: float
    adc3: float
    temps: list[float]
    bus_v: float
    bus_i: float
    bus_p: float
    adc1_cal: Optional[float]
    adc2_cal: Optional[float]
    adc3_cal: Optional[float]
    brightness_temp1: Optional[float] = None
    brightness_temp2: Optional[float] = None
    brightness_temp3: Optional[float] = None


@dataclass
class RadiometerCalibration:
    id: str
    device_id: str
    created_at: datetime
    t_black_body_1: float
    t_black_body_2: float
    adc1_1: float
    adc2_1: float
    adc3_1: float
    adc1_2: float
    adc2_2: float
    adc3_2: float
    t_adc1: float
    t_adc2: float
    t_adc3: float
    adc1_slope: float
    adc2_slope: float
    adc3_slope: float
    adc1_intercept: float
    adc2_intercept: float
    adc3_intercept: float
    comment: Optional[str]


@dataclass
class User:
    id: str
    username: str
    password_hash: str
    created_at: datetime


@dataclass
class AccessToken:
    token: str
    user_id: str
    created_at: datetime
    expires_at: datetime


@dataclass
class ErrorEvent:
    id: str
    device_id: str
    timestamp: datetime
    timestamp_ms: Optional[int]
    code: str
    severity: str
    message: str
    active: bool
    created_at: datetime


@dataclass
class Station:
    id: str
    station_id: str
    name: Optional[str]
    lat: Optional[float]
    lon: Optional[float]
    src: Optional[str]
    updated_at: Optional[datetime]
    created_at: datetime


@dataclass
class Sounding:
    id: str
    station_id: str
    sounding_time: datetime
    station_name: Optional[str]
    columns: list[str]
    rows: list[list[object]]
    units: dict[str, str]
    raw_text: str
    row_count: int
    fetched_at: datetime


@dataclass
class SoundingJob:
    id: str
    station_id: str
    status: str
    start_at: datetime
    end_at: datetime
    step_hours: int
    total: int
    done: int
    error: Optional[str]
    created_at: datetime
    updated_at: datetime


@dataclass
class SoundingExportJob:
    id: str
    station_id: str
    status: str
    sounding_ids: list[str]
    total: int
    done: int
    error: Optional[str]
    file_path: Optional[str]
    file_name: Optional[str]
    created_at: datetime
    updated_at: datetime


@dataclass
class SoundingScheduleItem:
    id: str
    station_id: str
    station_code: str
    station_name: Optional[str]
    enabled: bool
    created_at: datetime


@dataclass
class SoundingScheduleConfig:
    id: int
    interval_hours: int
    offset_hours: int
    updated_at: datetime
