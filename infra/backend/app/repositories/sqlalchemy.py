from __future__ import annotations

from datetime import datetime
from typing import Sequence

from sqlalchemy import delete, func, or_, select, update
from sqlalchemy.ext.asyncio import AsyncSession

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
from app.db.models import (
    AccessTokenModel,
    DeviceModel,
    ErrorEventModel,
    MeasurementModel,
    SoundingExportJobModel,
    SoundingJobModel,
    SoundingModel,
    SoundingScheduleConfigModel,
    SoundingScheduleModel,
    StationModel,
    UserModel,
)
from app.repositories.interfaces import (
    DeviceRepository,
    ErrorRepository,
    MeasurementRepository,
    SoundingJobRepository,
    SoundingExportJobRepository,
    SoundingRepository,
    SoundingScheduleConfigRepository,
    SoundingScheduleRepository,
    StationRepository,
    TokenRepository,
    UserRepository,
)


def to_device(model: DeviceModel) -> Device:
    return Device(
        id=model.id,
        display_name=model.display_name,
        created_at=model.created_at,
        last_seen_at=model.last_seen_at,
        temp_labels=list(model.temp_labels or []),
        temp_addresses=list(model.temp_addresses or []),
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


def to_station(model: StationModel) -> Station:
    return Station(
        id=model.id,
        station_id=model.station_id,
        name=model.name,
        lat=model.lat,
        lon=model.lon,
        src=model.src,
        updated_at=model.updated_at,
        created_at=model.created_at,
    )


def to_sounding(model: SoundingModel) -> Sounding:
    return Sounding(
        id=model.id,
        station_id=model.station_id,
        sounding_time=model.sounding_time,
        station_name=model.station_name,
        columns=list(model.columns or []),
        rows=list(model.rows or []),
        units=dict(model.units or {}),
        raw_text=model.raw_text,
        row_count=model.row_count,
        fetched_at=model.fetched_at,
    )


def to_sounding_job(model: SoundingJobModel) -> SoundingJob:
    return SoundingJob(
        id=model.id,
        station_id=model.station_id,
        status=model.status,
        start_at=model.start_at,
        end_at=model.end_at,
        step_hours=model.step_hours,
        total=model.total,
        done=model.done,
        error=model.error,
        created_at=model.created_at,
        updated_at=model.updated_at,
    )


def to_sounding_export_job(model: SoundingExportJobModel) -> SoundingExportJob:
    return SoundingExportJob(
        id=model.id,
        station_id=model.station_id,
        status=model.status,
        sounding_ids=list(model.sounding_ids or []),
        total=model.total,
        done=model.done,
        error=model.error,
        file_path=model.file_path,
        file_name=model.file_name,
        created_at=model.created_at,
        updated_at=model.updated_at,
    )


def to_sounding_schedule_item(
    schedule: SoundingScheduleModel,
    station: StationModel,
) -> SoundingScheduleItem:
    return SoundingScheduleItem(
        id=schedule.id,
        station_id=schedule.station_id,
        station_code=station.station_id,
        station_name=station.name,
        enabled=schedule.enabled,
        created_at=schedule.created_at,
    )


def to_sounding_schedule_config(model: SoundingScheduleConfigModel) -> SoundingScheduleConfig:
    return SoundingScheduleConfig(
        id=model.id,
        interval_hours=model.interval_hours,
        offset_hours=model.offset_hours,
        updated_at=model.updated_at,
    )


def to_token(model: AccessTokenModel) -> AccessToken:
    return AccessToken(
        token=model.token,
        user_id=model.user_id,
        created_at=model.created_at,
        expires_at=model.expires_at,
    )


def to_error_event(model: ErrorEventModel) -> ErrorEvent:
    return ErrorEvent(
        id=model.id,
        device_id=model.device_id,
        timestamp=model.timestamp,
        timestamp_ms=model.timestamp_ms,
        code=model.code,
        severity=model.severity,
        message=model.message,
        active=model.active,
        created_at=model.created_at,
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
        model = DeviceModel(id=device_id, display_name=display_name, temp_labels=[], temp_addresses=[], adc_labels={})
        self._session.add(model)
        await self._session.flush()
        return to_device(model)

    async def touch(self, device_id: str, seen_at: datetime) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if model is None:
            model = DeviceModel(id=device_id, last_seen_at=seen_at, temp_labels=[], temp_addresses=[], adc_labels={})
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
        temp_addresses: list[str] | None,
        adc_labels: dict[str, str] | None,
    ) -> Device:
        result = await self._session.execute(select(DeviceModel).where(DeviceModel.id == device_id))
        model = result.scalar_one_or_none()
        if not model:
            model = DeviceModel(
                id=device_id,
                display_name=display_name,
                temp_labels=temp_labels or [],
                temp_addresses=temp_addresses or [],
                adc_labels=adc_labels or {},
            )
            self._session.add(model)
        else:
            if display_name is not None:
                model.display_name = display_name
            if temp_labels is not None:
                model.temp_labels = temp_labels
            if temp_addresses is not None:
                model.temp_addresses = temp_addresses
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


class SqlStationRepository(StationRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def list(self, limit: int, offset: int, query: str | None) -> Sequence[Station]:
        stmt = select(StationModel).order_by(StationModel.station_id.asc()).offset(offset).limit(limit)
        if query:
            pattern = f"%{query}%"
            stmt = stmt.where(or_(StationModel.station_id.ilike(pattern), StationModel.name.ilike(pattern)))
        result = await self._session.execute(stmt)
        return [to_station(row) for row in result.scalars().all()]

    async def count(self, query: str | None) -> int:
        stmt = select(func.count()).select_from(StationModel)
        if query:
            pattern = f"%{query}%"
            stmt = stmt.where(or_(StationModel.station_id.ilike(pattern), StationModel.name.ilike(pattern)))
        result = await self._session.execute(stmt)
        return int(result.scalar_one())

    async def get(self, station_id: str) -> Station | None:
        result = await self._session.execute(select(StationModel).where(StationModel.station_id == station_id))
        model = result.scalar_one_or_none()
        return to_station(model) if model else None

    async def get_by_internal_id(self, station_id: str) -> Station | None:
        result = await self._session.execute(select(StationModel).where(StationModel.id == station_id))
        model = result.scalar_one_or_none()
        return to_station(model) if model else None

    async def upsert(
        self,
        station_id: str,
        name: str | None,
        lat: float | None,
        lon: float | None,
        src: str | None,
        updated_at: datetime,
    ) -> Station:
        result = await self._session.execute(select(StationModel).where(StationModel.station_id == station_id))
        model = result.scalar_one_or_none()
        if not model:
            model = StationModel(
                station_id=station_id,
                name=name,
                lat=lat,
                lon=lon,
                src=src,
                updated_at=updated_at,
            )
            self._session.add(model)
        else:
            if name is not None:
                model.name = name
            if lat is not None:
                model.lat = lat
            if lon is not None:
                model.lon = lon
            if src is not None:
                model.src = src
            model.updated_at = updated_at
        await self._session.flush()
        await self._session.refresh(model)
        return to_station(model)


class SqlSoundingRepository(SoundingRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def list(
        self,
        station_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        offset: int,
    ) -> Sequence[Sounding]:
        query = select(SoundingModel).where(SoundingModel.station_id == station_id)
        if start:
            query = query.where(SoundingModel.sounding_time >= start)
        if end:
            query = query.where(SoundingModel.sounding_time <= end)
        query = query.order_by(SoundingModel.sounding_time.desc()).offset(offset).limit(limit)
        result = await self._session.execute(query)
        return [to_sounding(row) for row in result.scalars().all()]

    async def count(self, station_id: str, start: datetime | None, end: datetime | None) -> int:
        query = select(func.count()).select_from(SoundingModel).where(SoundingModel.station_id == station_id)
        if start:
            query = query.where(SoundingModel.sounding_time >= start)
        if end:
            query = query.where(SoundingModel.sounding_time <= end)
        result = await self._session.execute(query)
        return int(result.scalar_one())

    async def get(self, sounding_id: str) -> Sounding | None:
        result = await self._session.execute(select(SoundingModel).where(SoundingModel.id == sounding_id))
        model = result.scalar_one_or_none()
        return to_sounding(model) if model else None

    async def get_many(self, sounding_ids: Sequence[str]) -> Sequence[Sounding]:
        ids = list(dict.fromkeys([sid for sid in sounding_ids if sid]))
        if not ids:
            return []
        result = await self._session.execute(select(SoundingModel).where(SoundingModel.id.in_(ids)))
        return [to_sounding(row) for row in result.scalars().all()]

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
        result = await self._session.execute(
            select(SoundingModel).where(
                SoundingModel.station_id == station_id,
                SoundingModel.sounding_time == sounding_time,
            )
        )
        model = result.scalar_one_or_none()
        if not model:
            model = SoundingModel(
                station_id=station_id,
                sounding_time=sounding_time,
                station_name=station_name,
                columns=columns,
                rows=rows,
                units=units,
                raw_text=raw_text,
                row_count=row_count,
            )
            self._session.add(model)
        else:
            model.station_name = station_name
            model.columns = columns
            model.rows = rows
            model.units = units
            model.raw_text = raw_text
            model.row_count = row_count
        await self._session.flush()
        await self._session.refresh(model)
        return to_sounding(model)


class SqlSoundingJobRepository(SoundingJobRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def create(
        self,
        station_id: str,
        start_at: datetime,
        end_at: datetime,
        step_hours: int,
    ) -> SoundingJob:
        model = SoundingJobModel(
            station_id=station_id,
            status="pending",
            start_at=start_at,
            end_at=end_at,
            step_hours=step_hours,
            total=0,
            done=0,
        )
        self._session.add(model)
        await self._session.flush()
        await self._session.refresh(model)
        return to_sounding_job(model)

    async def get(self, job_id: str) -> SoundingJob | None:
        result = await self._session.execute(select(SoundingJobModel).where(SoundingJobModel.id == job_id))
        model = result.scalar_one_or_none()
        return to_sounding_job(model) if model else None

    async def update_progress(
        self,
        job_id: str,
        status: str | None,
        total: int | None,
        done: int | None,
        error: str | None,
    ) -> SoundingJob:
        result = await self._session.execute(select(SoundingJobModel).where(SoundingJobModel.id == job_id))
        model = result.scalar_one_or_none()
        if not model:
            raise ValueError("Job not found")
        if status is not None:
            model.status = status
        if total is not None:
            model.total = total
        if done is not None:
            model.done = done
        if error is not None:
            model.error = error
        await self._session.flush()
        await self._session.refresh(model)
        await self._session.commit()
        return to_sounding_job(model)


class SqlSoundingExportJobRepository(SoundingExportJobRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def create(self, station_id: str, sounding_ids: list[str]) -> SoundingExportJob:
        model = SoundingExportJobModel(
            station_id=station_id,
            status="pending",
            sounding_ids=list(sounding_ids),
            total=0,
            done=0,
        )
        self._session.add(model)
        await self._session.flush()
        await self._session.refresh(model)
        return to_sounding_export_job(model)

    async def get(self, job_id: str) -> SoundingExportJob | None:
        result = await self._session.execute(select(SoundingExportJobModel).where(SoundingExportJobModel.id == job_id))
        model = result.scalar_one_or_none()
        return to_sounding_export_job(model) if model else None

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
        result = await self._session.execute(select(SoundingExportJobModel).where(SoundingExportJobModel.id == job_id))
        model = result.scalar_one_or_none()
        if not model:
            raise ValueError("Export job not found")
        if status is not None:
            model.status = status
        if total is not None:
            model.total = total
        if done is not None:
            model.done = done
        if error is not None:
            model.error = error
        if file_path is not None:
            model.file_path = file_path
        if file_name is not None:
            model.file_name = file_name
        await self._session.flush()
        await self._session.refresh(model)
        await self._session.commit()
        return to_sounding_export_job(model)


class SqlSoundingScheduleRepository(SoundingScheduleRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def list(self) -> Sequence[SoundingScheduleItem]:
        result = await self._session.execute(
            select(SoundingScheduleModel, StationModel)
            .join(StationModel, StationModel.id == SoundingScheduleModel.station_id)
            .order_by(StationModel.station_id.asc())
        )
        return [to_sounding_schedule_item(schedule, station) for schedule, station in result.all()]

    async def add(self, station_id: str) -> SoundingScheduleItem:
        result = await self._session.execute(
            select(SoundingScheduleModel).where(SoundingScheduleModel.station_id == station_id)
        )
        model = result.scalar_one_or_none()
        if not model:
            model = SoundingScheduleModel(station_id=station_id, enabled=True)
            self._session.add(model)
            await self._session.flush()
            await self._session.refresh(model)
        station = await self._session.get(StationModel, station_id)
        return to_sounding_schedule_item(model, station)

    async def set_enabled(self, schedule_id: str, enabled: bool) -> SoundingScheduleItem:
        result = await self._session.execute(select(SoundingScheduleModel).where(SoundingScheduleModel.id == schedule_id))
        model = result.scalar_one_or_none()
        if not model:
            raise ValueError("Schedule not found")
        model.enabled = enabled
        await self._session.flush()
        await self._session.refresh(model)
        station = await self._session.get(StationModel, model.station_id)
        return to_sounding_schedule_item(model, station)

    async def delete(self, schedule_id: str) -> None:
        await self._session.execute(delete(SoundingScheduleModel).where(SoundingScheduleModel.id == schedule_id))


class SqlSoundingScheduleConfigRepository(SoundingScheduleConfigRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def get(self) -> SoundingScheduleConfig:
        result = await self._session.execute(select(SoundingScheduleConfigModel).where(SoundingScheduleConfigModel.id == 1))
        model = result.scalar_one_or_none()
        if not model:
            model = SoundingScheduleConfigModel(id=1, interval_hours=3, offset_hours=2)
            self._session.add(model)
            await self._session.flush()
            await self._session.refresh(model)
        return to_sounding_schedule_config(model)

    async def update(self, interval_hours: int | None, offset_hours: int | None) -> SoundingScheduleConfig:
        result = await self._session.execute(select(SoundingScheduleConfigModel).where(SoundingScheduleConfigModel.id == 1))
        model = result.scalar_one_or_none()
        if not model:
            model = SoundingScheduleConfigModel(id=1, interval_hours=3, offset_hours=2)
            self._session.add(model)
        if interval_hours is not None:
            model.interval_hours = interval_hours
        if offset_hours is not None:
            model.offset_hours = offset_hours
        await self._session.flush()
        await self._session.refresh(model)
        return to_sounding_schedule_config(model)


class SqlErrorRepository(ErrorRepository):
    def __init__(self, session: AsyncSession) -> None:
        self._session = session

    async def add(self, event: ErrorEvent) -> None:
        model = ErrorEventModel(
            id=event.id,
            device_id=event.device_id,
            timestamp=event.timestamp,
            timestamp_ms=event.timestamp_ms,
            code=event.code,
            severity=event.severity,
            message=event.message,
            active=event.active,
        )
        self._session.add(model)
        await self._session.flush()

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
        query = select(ErrorEventModel).where(ErrorEventModel.device_id == device_id)
        if start:
            query = query.where(ErrorEventModel.timestamp >= start)
        if end:
            query = query.where(ErrorEventModel.timestamp <= end)
        if active is not None:
            query = query.where(ErrorEventModel.active.is_(active))
        if code:
            query = query.where(ErrorEventModel.code.ilike(f"%{code}%"))
        query = query.order_by(ErrorEventModel.timestamp.desc()).offset(offset).limit(limit)
        result = await self._session.execute(query)
        return [to_error_event(row) for row in result.scalars().all()]

    async def count(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        active: bool | None,
        code: str | None,
    ) -> int:
        query = select(func.count()).select_from(ErrorEventModel).where(ErrorEventModel.device_id == device_id)
        if start:
            query = query.where(ErrorEventModel.timestamp >= start)
        if end:
            query = query.where(ErrorEventModel.timestamp <= end)
        if active is not None:
            query = query.where(ErrorEventModel.active.is_(active))
        if code:
            query = query.where(ErrorEventModel.code.ilike(f"%{code}%"))
        result = await self._session.execute(query)
        return int(result.scalar_one())


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
