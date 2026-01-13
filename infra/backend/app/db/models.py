from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import Boolean, DateTime, Float, ForeignKey, Integer, String, Text, func
from sqlalchemy.dialects.postgresql import ARRAY
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

    measurements: Mapped[list[MeasurementModel]] = relationship("MeasurementModel", back_populates="device")


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

    user: Mapped[UserModel] = relationship("UserModel", back_populates="tokens")
