from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import Boolean, DateTime, Float, ForeignKey, Integer, String, Text, UniqueConstraint, func
from sqlalchemy.dialects.postgresql import ARRAY, JSONB
from sqlalchemy.orm import Mapped, mapped_column, relationship

from .base import Base


def new_uuid() -> str:
    return str(uuid.uuid4())


class DeviceModel(Base):
    __tablename__ = "devices"

    id: Mapped[str] = mapped_column(String(64), primary_key=True)
    display_name: Mapped[str | None] = mapped_column(String(128), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    temp_labels: Mapped[list[str]] = mapped_column(JSONB, default=list)
    temp_addresses: Mapped[list[str]] = mapped_column(JSONB, default=list)
    adc_labels: Mapped[dict[str, str]] = mapped_column(JSONB, default=dict)

    measurements: Mapped[list[MeasurementModel]] = relationship("MeasurementModel", back_populates="device")


class StationModel(Base):
    __tablename__ = "stations"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    station_id: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    name: Mapped[str | None] = mapped_column(Text, nullable=True)
    lat: Mapped[float | None] = mapped_column(Float, nullable=True)
    lon: Mapped[float | None] = mapped_column(Float, nullable=True)
    src: Mapped[str | None] = mapped_column(String(64), nullable=True)
    updated_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())


class MeasurementModel(Base):
    __tablename__ = "measurements"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), index=True)
    timestamp: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    timestamp_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    adc1: Mapped[float] = mapped_column(Float)
    adc2: Mapped[float] = mapped_column(Float)
    adc3: Mapped[float] = mapped_column(Float)
    temps: Mapped[list[float]] = mapped_column(ARRAY(Float), default=list)
    bus_v: Mapped[float] = mapped_column(Float)
    bus_i: Mapped[float] = mapped_column(Float)
    bus_p: Mapped[float] = mapped_column(Float)
    adc1_cal: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_cal: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_cal: Mapped[float | None] = mapped_column(Float, nullable=True)
    log_use_motor: Mapped[bool] = mapped_column(Boolean, default=False)
    log_duration: Mapped[float] = mapped_column(Float, default=1.0)
    log_filename: Mapped[str | None] = mapped_column(Text, nullable=True)

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="measurements")


class ErrorEventModel(Base):
    __tablename__ = "error_events"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), index=True)
    timestamp: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    timestamp_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    code: Mapped[str] = mapped_column(String(64))
    severity: Mapped[str] = mapped_column(String(16))
    message: Mapped[str] = mapped_column(Text)
    active: Mapped[bool] = mapped_column(Boolean, default=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), index=True)


class UserModel(Base):
    __tablename__ = "users"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    username: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    password_hash: Mapped[str] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())

    tokens: Mapped[list[AccessTokenModel]] = relationship("AccessTokenModel", back_populates="user")


class AccessTokenModel(Base):
    __tablename__ = "access_tokens"

    token: Mapped[str] = mapped_column(String(128), primary_key=True)
    user_id: Mapped[str] = mapped_column(String(36), ForeignKey("users.id"), index=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)

    user: Mapped[UserModel] = relationship("UserModel", back_populates="tokens")


class SoundingModel(Base):
    __tablename__ = "soundings"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    station_id: Mapped[str] = mapped_column(String(36), ForeignKey("stations.id"), index=True)
    sounding_time: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    station_name: Mapped[str | None] = mapped_column(Text, nullable=True)
    columns: Mapped[list[str]] = mapped_column(JSONB, default=list)
    rows: Mapped[list[list[object]]] = mapped_column(JSONB, default=list)
    units: Mapped[dict[str, str]] = mapped_column(JSONB, default=dict)
    raw_text: Mapped[str] = mapped_column(Text)
    row_count: Mapped[int] = mapped_column(Integer, default=0)
    fetched_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), index=True)

    __table_args__ = (UniqueConstraint("station_id", "sounding_time", name="uq_soundings_station_time"),)


class SoundingJobModel(Base):
    __tablename__ = "sounding_jobs"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    station_id: Mapped[str] = mapped_column(String(36), ForeignKey("stations.id"), index=True)
    status: Mapped[str] = mapped_column(String(16), index=True)
    start_at: Mapped[datetime] = mapped_column(DateTime(timezone=True))
    end_at: Mapped[datetime] = mapped_column(DateTime(timezone=True))
    step_hours: Mapped[int] = mapped_column(Integer)
    total: Mapped[int] = mapped_column(Integer, default=0)
    done: Mapped[int] = mapped_column(Integer, default=0)
    error: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())


class SoundingExportJobModel(Base):
    __tablename__ = "sounding_export_jobs"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    station_id: Mapped[str] = mapped_column(String(36), ForeignKey("stations.id"), index=True)
    status: Mapped[str] = mapped_column(String(16), index=True)
    sounding_ids: Mapped[list[str]] = mapped_column(JSONB, default=list)
    total: Mapped[int] = mapped_column(Integer, default=0)
    done: Mapped[int] = mapped_column(Integer, default=0)
    error: Mapped[str | None] = mapped_column(Text, nullable=True)
    file_path: Mapped[str | None] = mapped_column(Text, nullable=True)
    file_name: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())


class SoundingScheduleModel(Base):
    __tablename__ = "sounding_schedule"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    station_id: Mapped[str] = mapped_column(String(36), ForeignKey("stations.id"), unique=True, index=True)
    enabled: Mapped[bool] = mapped_column(Boolean, default=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())


class SoundingScheduleConfigModel(Base):
    __tablename__ = "sounding_schedule_config"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    interval_hours: Mapped[int] = mapped_column(Integer, default=3)
    offset_hours: Mapped[int] = mapped_column(Integer, default=2)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())
