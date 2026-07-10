from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import BigInteger, Boolean, DateTime, Float, ForeignKey, Index, Integer, String, Text, UniqueConstraint, func
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
    temp_label_map: Mapped[dict[str, str]] = mapped_column(JSONB, default=dict)
    temp_bindings: Mapped[dict[str, str]] = mapped_column(JSONB, default=dict)
    atmosphere_config: Mapped[dict[str, object]] = mapped_column(JSONB, default=dict)
    adc_labels: Mapped[dict[str, str]] = mapped_column(JSONB, default=dict)
    has_meteo: Mapped[bool] = mapped_column(Boolean, default=False)

    measurements: Mapped[list[MeasurementModel]] = relationship("MeasurementModel", back_populates="device")
    gnss_data_sets: Mapped[list[GnssDataModel]] = relationship(
        "GnssDataModel", back_populates="device", cascade="all, delete-orphan"
    )
    radiometer_calibrations: Mapped[list[RadiometerCalibrationModel]] = relationship(
        "RadiometerCalibrationModel", back_populates="device"
    )
    meteo_readings: Mapped[list[MeteoReadingModel]] = relationship(
        "MeteoReadingModel", back_populates="device", cascade="all, delete-orphan"
    )
    gps_config: Mapped[DeviceGpsConfigModel | None] = relationship(
        "DeviceGpsConfigModel", back_populates="device", uselist=False
    )


class DeviceGpsConfigModel(Base):
    __tablename__ = "device_gps_configs"

    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), primary_key=True)
    has_gps: Mapped[bool] = mapped_column(Boolean, default=False)
    rtcm_types: Mapped[list[int]] = mapped_column(JSONB, default=lambda: [1004, 1006, 1033])
    mode: Mapped[str] = mapped_column(String(32), default="base_time_60")
    actual_mode: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="gps_config")


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


class GnssDataModel(Base):
    __tablename__ = "gnss_data"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id", ondelete="CASCADE"), index=True)
    name: Mapped[str] = mapped_column(String(128))
    description: Mapped[str | None] = mapped_column(Text, nullable=True)
    measurement_count: Mapped[int] = mapped_column(Integer, default=0)
    start_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    end_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_import_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="gnss_data_sets")
    measurements: Mapped[list[GnssDataMeasurementModel]] = relationship(
        "GnssDataMeasurementModel", back_populates="gnss_data", cascade="all, delete-orphan"
    )

    __table_args__ = (UniqueConstraint("device_id", "name", name="uq_gnss_data_device_name"),)


class GnssDataMeasurementModel(Base):
    __tablename__ = "gnss_data_measurements"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    gnss_data_id: Mapped[str] = mapped_column(String(36), ForeignKey("gnss_data.id", ondelete="CASCADE"))
    measured_at: Mapped[datetime] = mapped_column(DateTime(timezone=True))
    pw_mm: Mapped[float] = mapped_column(Float)
    spw_mm: Mapped[float | None] = mapped_column(Float, nullable=True)
    temperature_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now())

    gnss_data: Mapped[GnssDataModel] = relationship("GnssDataModel", back_populates="measurements")

    __table_args__ = (
        UniqueConstraint("gnss_data_id", "measured_at", name="uq_gnss_data_measurement_time"),
        Index("ix_gnss_data_measurements_dataset_time", "gnss_data_id", "measured_at"),
    )


class MeasurementModel(Base):
    __tablename__ = "measurements"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), index=True)
    timestamp: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    timestamp_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
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
    gps_lat: Mapped[float | None] = mapped_column(Float, nullable=True)
    gps_lon: Mapped[float | None] = mapped_column(Float, nullable=True)
    gps_alt: Mapped[float | None] = mapped_column(Float, nullable=True)
    gps_fix_quality: Mapped[int | None] = mapped_column(Integer, nullable=True)
    gps_satellites: Mapped[int | None] = mapped_column(Integer, nullable=True)
    gps_fix_age_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    log_use_motor: Mapped[bool] = mapped_column(Boolean, default=False)
    log_duration: Mapped[float] = mapped_column(Float, default=1.0)
    log_filename: Mapped[str | None] = mapped_column(Text, nullable=True)
    meteo_reading_id: Mapped[str | None] = mapped_column(
        String(36), ForeignKey("meteo_readings.id", ondelete="SET NULL"), nullable=True, index=True
    )

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="measurements")
    meteo_reading: Mapped[MeteoReadingModel | None] = relationship("MeteoReadingModel")


class MeteoReadingModel(Base):
    __tablename__ = "meteo_readings"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id", ondelete="CASCADE"), index=True)
    timestamp: Mapped[datetime] = mapped_column(DateTime(timezone=True))
    timestamp_ms: Mapped[int] = mapped_column(BigInteger)
    temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    humidity_pct: Mapped[float | None] = mapped_column(Float, nullable=True)
    wind_speed_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    gust_speed_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    wind_dir_deg: Mapped[int | None] = mapped_column(Integer, nullable=True)
    pressure_hpa: Mapped[float | None] = mapped_column(Float, nullable=True)
    rainfall_mm: Mapped[float | None] = mapped_column(Float, nullable=True)
    light_lux: Mapped[float | None] = mapped_column(Float, nullable=True)
    uvi: Mapped[float | None] = mapped_column(Float, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="meteo_readings")

    __table_args__ = (
        UniqueConstraint("device_id", "timestamp_ms", name="uq_meteo_reading_device_time"),
        Index("ix_meteo_readings_device_time", "device_id", "timestamp"),
    )


class RadiometerCalibrationModel(Base):
    __tablename__ = "radiometer_calibrations"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), index=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), index=True)
    t_black_body_1: Mapped[float] = mapped_column(Float)
    t_black_body_2: Mapped[float] = mapped_column(Float)
    adc1_1: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_1: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_1: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc1_2: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_2: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_2: Mapped[float | None] = mapped_column(Float, nullable=True)
    t_adc1: Mapped[float] = mapped_column(Float)
    t_adc2: Mapped[float] = mapped_column(Float)
    t_adc3: Mapped[float] = mapped_column(Float)
    adc1_slope: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_slope: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_slope: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc1_intercept: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_intercept: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_intercept: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc1_noise_temp: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc2_noise_temp: Mapped[float | None] = mapped_column(Float, nullable=True)
    adc3_noise_temp: Mapped[float | None] = mapped_column(Float, nullable=True)
    comment: Mapped[str | None] = mapped_column(Text, nullable=True)

    device: Mapped[DeviceModel] = relationship("DeviceModel", back_populates="radiometer_calibrations")


class ErrorEventModel(Base):
    __tablename__ = "error_events"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.id"), index=True)
    timestamp: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    timestamp_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
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
