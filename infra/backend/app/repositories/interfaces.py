from __future__ import annotations

from abc import ABC, abstractmethod
from datetime import datetime
from typing import Iterable, Sequence

from app.domain.entities import (
    AccessToken,
    Device,
    ErrorEvent,
    Measurement,
    MeasurementPoint,
    Sounding,
    SoundingExportJob,
    SoundingJob,
    SoundingScheduleConfig,
    SoundingScheduleItem,
    Station,
    User,
)


class DeviceRepository(ABC):
    @abstractmethod
    async def list(self) -> Sequence[Device]:
        raise NotImplementedError

    @abstractmethod
    async def get(self, device_id: str) -> Device | None:
        raise NotImplementedError

    @abstractmethod
    async def create(self, device_id: str, display_name: str | None = None) -> Device:
        raise NotImplementedError

    @abstractmethod
    async def touch(self, device_id: str, seen_at: datetime) -> Device:
        raise NotImplementedError

    @abstractmethod
    async def update(
        self,
        device_id: str,
        display_name: str | None,
        temp_labels: list[str] | None,
        temp_addresses: list[str] | None,
        adc_labels: dict[str, str] | None,
    ) -> Device:
        raise NotImplementedError


class StationRepository(ABC):
    @abstractmethod
    async def list(self, limit: int, offset: int, query: str | None) -> Sequence[Station]:
        raise NotImplementedError

    @abstractmethod
    async def count(self, query: str | None) -> int:
        raise NotImplementedError

    @abstractmethod
    async def get(self, station_id: str) -> Station | None:
        raise NotImplementedError

    @abstractmethod
    async def get_by_internal_id(self, station_id: str) -> Station | None:
        raise NotImplementedError

    @abstractmethod
    async def upsert(
        self,
        station_id: str,
        name: str | None,
        lat: float | None,
        lon: float | None,
        src: str | None,
        updated_at: datetime,
    ) -> Station:
        raise NotImplementedError


class SoundingRepository(ABC):
    @abstractmethod
    async def list(
        self,
        station_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        offset: int,
    ) -> Sequence[Sounding]:
        raise NotImplementedError

    @abstractmethod
    async def count(self, station_id: str, start: datetime | None, end: datetime | None) -> int:
        raise NotImplementedError

    @abstractmethod
    async def get(self, sounding_id: str) -> Sounding | None:
        raise NotImplementedError

    @abstractmethod
    async def get_many(self, sounding_ids: Iterable[str]) -> Sequence[Sounding]:
        raise NotImplementedError

    @abstractmethod
    async def upsert(
        self,
        station_id: str,
        sounding_time: datetime,
        station_name: str | None,
        columns: list[str],
        rows: list[list[object]],
        units: dict[str, str],
        raw_text: str,
        row_count: int,
    ) -> Sounding:
        raise NotImplementedError


class SoundingJobRepository(ABC):
    @abstractmethod
    async def create(
        self,
        station_id: str,
        start_at: datetime,
        end_at: datetime,
        step_hours: int,
    ) -> SoundingJob:
        raise NotImplementedError

    @abstractmethod
    async def get(self, job_id: str) -> SoundingJob | None:
        raise NotImplementedError

    @abstractmethod
    async def update_progress(
        self,
        job_id: str,
        status: str | None,
        total: int | None,
        done: int | None,
        error: str | None,
    ) -> SoundingJob:
        raise NotImplementedError


class SoundingExportJobRepository(ABC):
    @abstractmethod
    async def create(self, station_id: str, sounding_ids: list[str]) -> SoundingExportJob:
        raise NotImplementedError

    @abstractmethod
    async def get(self, job_id: str) -> SoundingExportJob | None:
        raise NotImplementedError

    @abstractmethod
    async def update_progress(
        self,
        job_id: str,
        status: str | None,
        total: int | None,
        done: int | None,
        error: str | None,
        file_path: str | None,
        file_name: str | None,
    ) -> SoundingExportJob:
        raise NotImplementedError


class SoundingScheduleRepository(ABC):
    @abstractmethod
    async def list(self) -> Sequence[SoundingScheduleItem]:
        raise NotImplementedError

    @abstractmethod
    async def add(self, station_id: str) -> SoundingScheduleItem:
        raise NotImplementedError

    @abstractmethod
    async def set_enabled(self, schedule_id: str, enabled: bool) -> SoundingScheduleItem:
        raise NotImplementedError

    @abstractmethod
    async def delete(self, schedule_id: str) -> None:
        raise NotImplementedError


class SoundingScheduleConfigRepository(ABC):
    @abstractmethod
    async def get(self) -> SoundingScheduleConfig:
        raise NotImplementedError

    @abstractmethod
    async def update(self, interval_hours: int | None, offset_hours: int | None) -> SoundingScheduleConfig:
        raise NotImplementedError


class MeasurementRepository(ABC):
    @abstractmethod
    async def add(self, measurement: Measurement) -> None:
        raise NotImplementedError

    @abstractmethod
    async def list(self, device_id: str, start: datetime | None, end: datetime | None, limit: int) -> Sequence[Measurement]:
        raise NotImplementedError

    @abstractmethod
    async def count(self, device_id: str, start: datetime | None, end: datetime | None) -> int:
        raise NotImplementedError

    @abstractmethod
    async def bounds(self, device_id: str, start: datetime | None, end: datetime | None) -> tuple[datetime | None, datetime | None]:
        raise NotImplementedError

    @abstractmethod
    async def list_aggregated(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        bucket_seconds: int,
        limit: int,
    ) -> Sequence[MeasurementPoint]:
        raise NotImplementedError


class UserRepository(ABC):
    @abstractmethod
    async def list(self) -> Sequence[User]:
        raise NotImplementedError

    @abstractmethod
    async def count(self) -> int:
        raise NotImplementedError

    @abstractmethod
    async def get_by_username(self, username: str) -> User | None:
        raise NotImplementedError

    @abstractmethod
    async def get(self, user_id: str) -> User | None:
        raise NotImplementedError

    @abstractmethod
    async def create(self, username: str, password_hash: str) -> User:
        raise NotImplementedError

    @abstractmethod
    async def update(self, user_id: str, username: str | None, password_hash: str | None) -> User:
        raise NotImplementedError

    @abstractmethod
    async def delete(self, user_id: str) -> None:
        raise NotImplementedError


class TokenRepository(ABC):
    @abstractmethod
    async def create(self, token: str, user_id: str, expires_at: datetime) -> AccessToken:
        raise NotImplementedError

    @abstractmethod
    async def get(self, token: str) -> AccessToken | None:
        raise NotImplementedError

    @abstractmethod
    async def delete(self, token: str) -> None:
        raise NotImplementedError


class ErrorRepository(ABC):
    @abstractmethod
    async def add(self, event: ErrorEvent) -> None:
        raise NotImplementedError

    @abstractmethod
    async def list(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        active: bool | None,
        code: str | None,
        limit: int,
        offset: int,
    ) -> Sequence[ErrorEvent]:
        raise NotImplementedError

    @abstractmethod
    async def count(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        active: bool | None,
        code: str | None,
    ) -> int:
        raise NotImplementedError
