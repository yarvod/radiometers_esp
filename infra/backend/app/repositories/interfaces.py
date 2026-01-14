from __future__ import annotations

from abc import ABC, abstractmethod
from datetime import datetime
from typing import Iterable, Sequence

from app.domain.entities import AccessToken, Device, Measurement, MeasurementPoint, User


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
