from __future__ import annotations

from datetime import datetime
from typing import Sequence

from sqlalchemy import delete, func, select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.domain.entities import AccessToken, Device, Measurement, MeasurementPoint, User
from app.db.models import AccessTokenModel, DeviceModel, MeasurementModel, UserModel
from app.repositories.interfaces import DeviceRepository, MeasurementRepository, TokenRepository, UserRepository


def to_device(model: DeviceModel) -> Device:
    return Device(
        id=model.id,
        display_name=model.display_name,
        created_at=model.created_at,
        last_seen_at=model.last_seen_at,
        temp_labels=list(model.temp_labels or []),
        temp_address_labels=dict(model.temp_address_labels or {}),
        adc_labels=dict(model.adc_labels or {}),
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


def to_point_from_measurement(model: MeasurementModel) -> MeasurementPoint:
    return MeasurementPoint(
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
        model = DeviceModel(id=device_id, display_name=display_name, temp_labels=[], temp_address_labels={}, adc_labels={})
        self._session.add(model)
        await self._session.flush()
        return to_device(model)

    async def touch(self, device_id: str, seen_at: datetime) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if model is None:
            model = DeviceModel(id=device_id, last_seen_at=seen_at, temp_labels=[], temp_address_labels={}, adc_labels={})
            self._session.add(model)
        else:
            model.last_seen_at = seen_at
        await self._session.flush()
        return to_device(model)

    async def update(
        self,
        device_id: str,
        display_name: str | None,
        temp_labels: list[str] | None,
        temp_address_labels: dict[str, str] | None,
        adc_labels: dict[str, str] | None,
    ) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if not model:
            model = DeviceModel(
                id=device_id,
                display_name=display_name,
                temp_labels=temp_labels or [],
                temp_address_labels=temp_address_labels or {},
                adc_labels=adc_labels or {},
            )
            self._session.add(model)
        else:
            if display_name is not None:
                model.display_name = display_name
            if temp_labels is not None:
                model.temp_labels = temp_labels
            if temp_address_labels is not None:
                model.temp_address_labels = temp_address_labels
            if adc_labels is not None:
                model.adc_labels = adc_labels
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

    async def count(self, device_id: str, start: datetime | None, end: datetime | None) -> int:
        query = select(func.count()).select_from(MeasurementModel).where(MeasurementModel.device_id == device_id)
        if start:
            query = query.where(MeasurementModel.timestamp >= start)
        if end:
            query = query.where(MeasurementModel.timestamp <= end)
        result = await self._session.execute(query)
        return int(result.scalar_one())

    async def bounds(self, device_id: str, start: datetime | None, end: datetime | None) -> tuple[datetime | None, datetime | None]:
        query = select(func.min(MeasurementModel.timestamp), func.max(MeasurementModel.timestamp)).where(
            MeasurementModel.device_id == device_id
        )
        if start:
            query = query.where(MeasurementModel.timestamp >= start)
        if end:
            query = query.where(MeasurementModel.timestamp <= end)
        result = await self._session.execute(query)
        row = result.one()
        return row[0], row[1]

    async def list_aggregated(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        bucket_seconds: int,
        limit: int,
    ) -> Sequence[MeasurementPoint]:
        bucket = func.floor(func.extract("epoch", MeasurementModel.timestamp) / bucket_seconds) * bucket_seconds
        bucket_ts = func.to_timestamp(bucket).label("bucket_ts")
        columns = [
            bucket_ts,
            func.avg(MeasurementModel.adc1).label("adc1"),
            func.avg(MeasurementModel.adc2).label("adc2"),
            func.avg(MeasurementModel.adc3).label("adc3"),
            func.avg(MeasurementModel.bus_v).label("bus_v"),
            func.avg(MeasurementModel.bus_i).label("bus_i"),
            func.avg(MeasurementModel.bus_p).label("bus_p"),
            func.avg(MeasurementModel.adc1_cal).label("adc1_cal"),
            func.avg(MeasurementModel.adc2_cal).label("adc2_cal"),
            func.avg(MeasurementModel.adc3_cal).label("adc3_cal"),
        ]
        max_temps = 8
        for idx in range(1, max_temps + 1):
            columns.append(func.avg(MeasurementModel.temps[idx]).label(f"temp{idx}"))

        query = select(*columns).where(MeasurementModel.device_id == device_id)
        if start:
            query = query.where(MeasurementModel.timestamp >= start)
        if end:
            query = query.where(MeasurementModel.timestamp <= end)
        query = query.group_by(bucket_ts).order_by(bucket_ts.asc()).limit(limit)

        result = await self._session.execute(query)
        points: list[MeasurementPoint] = []
        for row in result:
            temps = [getattr(row, f"temp{idx}") for idx in range(1, max_temps + 1)]
            while temps and temps[-1] is None:
                temps.pop()
            timestamp = row.bucket_ts
            points.append(
                MeasurementPoint(
                    timestamp=timestamp,
                    timestamp_ms=int(timestamp.timestamp() * 1000) if timestamp else None,
                    adc1=float(row.adc1 or 0.0),
                    adc2=float(row.adc2 or 0.0),
                    adc3=float(row.adc3 or 0.0),
                    temps=[float(v) for v in temps if v is not None],
                    bus_v=float(row.bus_v or 0.0),
                    bus_i=float(row.bus_i or 0.0),
                    bus_p=float(row.bus_p or 0.0),
                    adc1_cal=float(row.adc1_cal) if row.adc1_cal is not None else None,
                    adc2_cal=float(row.adc2_cal) if row.adc2_cal is not None else None,
                    adc3_cal=float(row.adc3_cal) if row.adc3_cal is not None else None,
                )
            )
        return points


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
