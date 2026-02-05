from __future__ import annotations

from datetime import datetime
from typing import Optional

from pydantic import BaseModel, ConfigDict, Field


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


class StationOut(BaseModel):
    id: str
    station_id: str
    name: Optional[str] = None
    lat: Optional[float] = None
    lon: Optional[float] = None
    src: Optional[str] = None
    updated_at: Optional[datetime] = None
    created_at: datetime


class StationUpdateRequest(BaseModel):
    name: Optional[str] = None
    lat: Optional[float] = None
    lon: Optional[float] = None
    src: Optional[str] = None


class StationRefreshRequest(BaseModel):
    model_config = ConfigDict(populate_by_name=True)
    requested_at: Optional[datetime] = Field(default=None, alias="datetime")


class StationsResponse(BaseModel):
    items: list[StationOut]
    total: int
    limit: int
    offset: int


class StationsRefreshResponse(BaseModel):
    model_config = ConfigDict(populate_by_name=True)
    requested_at: datetime = Field(alias="datetime")
    fetched: int
    updated: int


class SoundingOut(BaseModel):
    id: str
    station_id: str
    sounding_time: datetime
    station_name: Optional[str] = None
    row_count: int
    fetched_at: datetime


class SoundingDetailOut(BaseModel):
    id: str
    station_id: str
    sounding_time: datetime
    station_name: Optional[str] = None
    columns: list[str]
    rows: list[list[object]]
    units: dict[str, str]
    raw_text: str
    row_count: int
    fetched_at: datetime


class SoundingsResponse(BaseModel):
    items: list[SoundingOut]
    total: int
    limit: int
    offset: int


class SoundingJobCreateRequest(BaseModel):
    start_at: datetime
    end_at: datetime
    step_hours: int = Field(default=3, ge=1, le=24)


class SoundingExportRequest(BaseModel):
    ids: list[str] = Field(..., min_length=1)


class SoundingJobOut(BaseModel):
    id: str
    station_id: str
    status: str
    start_at: datetime
    end_at: datetime
    step_hours: int
    total: int
    done: int
    error: Optional[str] = None
    created_at: datetime
    updated_at: datetime


class SoundingExportJobOut(BaseModel):
    id: str
    station_id: str
    status: str
    sounding_ids: list[str]
    total: int
    done: int
    error: Optional[str] = None
    file_name: Optional[str] = None
    created_at: datetime
    updated_at: datetime


class SoundingScheduleItemOut(BaseModel):
    id: str
    station_id: str
    station_code: str
    station_name: Optional[str] = None
    enabled: bool
    created_at: datetime


class SoundingScheduleResponse(BaseModel):
    items: list[SoundingScheduleItemOut]


class SoundingScheduleCreateRequest(BaseModel):
    station_id: str = Field(..., min_length=1)


class SoundingScheduleUpdateRequest(BaseModel):
    enabled: bool


class SoundingScheduleConfigOut(BaseModel):
    id: int
    interval_hours: int
    offset_hours: int
    updated_at: datetime


class SoundingScheduleConfigUpdateRequest(BaseModel):
    interval_hours: Optional[int] = Field(default=None, ge=1, le=24)
    offset_hours: Optional[int] = Field(default=None, ge=0, le=23)


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
