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
