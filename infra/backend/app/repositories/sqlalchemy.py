from __future__ import annotations

from datetime import datetime
from typing import Sequence

from sqlalchemy import delete, func, select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.domain.entities import AccessToken, Device, Measurement, User
from app.db.models import AccessTokenModel, DeviceModel, MeasurementModel, UserModel
from app.repositories.interfaces import DeviceRepository, MeasurementRepository, TokenRepository, UserRepository


def to_device(model: DeviceModel) -> Device:
    return Device(
        id=model.id,
        display_name=model.display_name,
        created_at=model.created_at,
        last_seen_at=model.last_seen_at,
    )


def to_measurement(model: MeasurementModel) -> Measurement:
    return Measurement(
        id=model.id,
        device_id=model.device_id,
        timestamp=model.timestamp,
        timestamp_ms=model.timestamp_ms,
        adc1=model.adc1,
        adc2=model.adc2,
        adc3=model.adc3,
        temps=list(model.temps or []),
        bus_v=model.bus_v,
        bus_i=model.bus_i,
        bus_p=model.bus_p,
        adc1_cal=model.adc1_cal,
        adc2_cal=model.adc2_cal,
        adc3_cal=model.adc3_cal,
        log_use_motor=model.log_use_motor,
        log_duration=model.log_duration,
        log_filename=model.log_filename,
    )


def to_user(model: UserModel) -> User:
    return User(
        id=model.id,
        username=model.username,
        password_hash=model.password_hash,
        created_at=model.created_at,
    )


def to_token(model: AccessTokenModel) -> AccessToken:
    return AccessToken(
        token=model.token,
        user_id=model.user_id,
        created_at=model.created_at,
        expires_at=model.expires_at,
    )


class SqlDeviceRepository(DeviceRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def list(self) -> Sequence[Device]:
        result = await self._session.execute(select(DeviceModel).order_by(DeviceModel.id))
        return [to_device(row) for row in result.scalars().all()]

    async def get(self, device_id: str) -> Device | None:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        return to_device(model) if model else None

    async def create(self, device_id: str, display_name: str | None = None) -> Device:
        model = DeviceModel(id=device_id, display_name=display_name)
        self._session.add(model)
        await self._session.flush()
        return to_device(model)

    async def touch(self, device_id: str, seen_at: datetime) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if model is None:
            model = DeviceModel(id=device_id, last_seen_at=seen_at)
            self._session.add(model)
        else:
            model.last_seen_at = seen_at
        await self._session.flush()
        return to_device(model)

    async def update(self, device_id: str, display_name: str | None) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if not model:
            model = DeviceModel(id=device_id, display_name=display_name)
            self._session.add(model)
        else:
            model.display_name = display_name
        await self._session.flush()
        return to_device(model)


class SqlMeasurementRepository(MeasurementRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def add(self, measurement: Measurement) -> None:
        model = MeasurementModel(
            id=measurement.id,
            device_id=measurement.device_id,
            timestamp=measurement.timestamp,
            timestamp_ms=measurement.timestamp_ms,
            adc1=measurement.adc1,
            adc2=measurement.adc2,
            adc3=measurement.adc3,
            temps=measurement.temps,
            bus_v=measurement.bus_v,
            bus_i=measurement.bus_i,
            bus_p=measurement.bus_p,
            adc1_cal=measurement.adc1_cal,
            adc2_cal=measurement.adc2_cal,
            adc3_cal=measurement.adc3_cal,
            log_use_motor=measurement.log_use_motor,
            log_duration=measurement.log_duration,
            log_filename=measurement.log_filename,
        )
        self._session.add(model)
        await self._session.flush()

    async def list(self, device_id: str, start: datetime | None, end: datetime | None, limit: int) -> Sequence[Measurement]:
        query = select(MeasurementModel).where(MeasurementModel.device_id == device_id)
        if start:
            query = query.where(MeasurementModel.timestamp >= start)
        if end:
            query = query.where(MeasurementModel.timestamp <= end)
        query = query.order_by(MeasurementModel.timestamp.asc()).limit(limit)
        result = await self._session.execute(query)
        return [to_measurement(row) for row in result.scalars().all()]


class SqlUserRepository(UserRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def list(self) -> Sequence[User]:
        result = await self._session.execute(select(UserModel).order_by(UserModel.created_at.asc()))
        return [to_user(row) for row in result.scalars().all()]

    async def count(self) -> int:
        result = await self._session.execute(select(func.count()).select_from(UserModel))
        return int(result.scalar_one())

    async def get_by_username(self, username: str) -> User | None:
        result = await self._session.execute(select(UserModel).where(UserModel.username == username))
        model = result.scalar_one_or_none()
        return to_user(model) if model else None

    async def get(self, user_id: str) -> User | None:
        result = await self._session.execute(select(UserModel).where(UserModel.id == user_id))
        model = result.scalar_one_or_none()
        return to_user(model) if model else None

    async def create(self, username: str, password_hash: str) -> User:
        model = UserModel(username=username, password_hash=password_hash)
        self._session.add(model)
        await self._session.flush()
        return to_user(model)

    async def update(self, user_id: str, username: str | None, password_hash: str | None) -> User:
        result = await self._session.execute(select(UserModel).where(UserModel.id == user_id))
        model = result.scalar_one_or_none()
        if not model:
            raise ValueError("User not found")
        if username is not None:
            model.username = username
        if password_hash is not None:
            model.password_hash = password_hash
        await self._session.flush()
        return to_user(model)

    async def delete(self, user_id: str) -> None:
        await self._session.execute(delete(UserModel).where(UserModel.id == user_id))


class SqlTokenRepository(TokenRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def create(self, token: str, user_id: str, expires_at: datetime) -> AccessToken:
        model = AccessTokenModel(token=token, user_id=user_id, expires_at=expires_at)
        self._session.add(model)
        await self._session.flush()
        return to_token(model)

    async def get(self, token: str) -> AccessToken | None:
        result = await self._session.execute(select(AccessTokenModel).where(AccessTokenModel.token == token))
        model = result.scalar_one_or_none()
        return to_token(model) if model else None

    async def delete(self, token: str) -> None:
        await self._session.execute(delete(AccessTokenModel).where(AccessTokenModel.token == token))
