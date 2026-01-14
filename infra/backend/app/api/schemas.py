from __future__ import annotations

from datetime import datetime
from typing import Optional

from pydantic import BaseModel, Field


class UserOut(BaseModel):
    id: str
    username: str
    created_at: datetime


class AuthStatusResponse(BaseModel):
    has_users: bool
    user: Optional[UserOut] = None


class LoginRequest(BaseModel):
    username: str
    password: str


class SignupRequest(BaseModel):
    username: str
    password: str


class AuthResponse(BaseModel):
    access_token: str


class UserCreateRequest(BaseModel):
    username: str
    password: str


class UserUpdateRequest(BaseModel):
    username: Optional[str] = None
    password: Optional[str] = None


class DeviceCreateRequest(BaseModel):
    id: str = Field(..., min_length=1)
    display_name: Optional[str] = None


class DeviceUpdateRequest(BaseModel):
    display_name: Optional[str] = None
    temp_labels: Optional[list[str]] = None
    adc_labels: Optional[dict[str, str]] = None


class DeviceOut(BaseModel):
    id: str
    display_name: Optional[str] = None
    created_at: datetime
    last_seen_at: Optional[datetime] = None


class DeviceConfigOut(BaseModel):
    id: str
    display_name: Optional[str] = None
    created_at: datetime
    last_seen_at: Optional[datetime] = None
    temp_labels: list[str] = Field(default_factory=list)
    temp_addresses: list[str] = Field(default_factory=list)
    adc_labels: dict[str, str] = Field(default_factory=dict)


class MeasurementOut(BaseModel):
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


class MeasurementPointOut(BaseModel):
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


class MeasurementsResponse(BaseModel):
    points: list[MeasurementPointOut]
    raw_count: int
    limit: int
    bucket_seconds: int
    bucket_label: str
    aggregated: bool
    temp_labels: list[str] = Field(default_factory=list)
    adc_labels: dict[str, str] = Field(default_factory=dict)
    temp_addresses: list[str] = Field(default_factory=list)


class MeasurementLatestResponse(BaseModel):
    timestamp: Optional[datetime]
    timestamp_ms: Optional[int]


class ErrorEventOut(BaseModel):
    id: str
    device_id: str
    timestamp: datetime
    timestamp_ms: Optional[int]
    code: str
    severity: str
    message: str
    active: bool
    created_at: datetime


class ErrorEventsResponse(BaseModel):
    items: list[ErrorEventOut]
    total: int
    limit: int
    offset: int
