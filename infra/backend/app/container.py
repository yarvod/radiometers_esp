from __future__ import annotations

from typing import AsyncIterator

from dishka import Provider, Scope, provide
from sqlalchemy.ext.asyncio import AsyncEngine, AsyncSession, async_sessionmaker

from app.core.config import Settings
from app.db.session import create_engine, create_session_factory
from arq.connections import ArqRedis, RedisSettings, create_pool

from app.repositories.interfaces import (
    DeviceRepository,
    ErrorRepository,
    MeasurementRepository,
    SoundingExportJobRepository,
    SoundingJobRepository,
    SoundingRepository,
    SoundingScheduleConfigRepository,
    SoundingScheduleRepository,
    StationRepository,
    TokenRepository,
    UserRepository,
)
from app.repositories.sqlalchemy import (
    SqlDeviceRepository,
    SqlErrorRepository,
    SqlMeasurementRepository,
    SqlSoundingExportJobRepository,
    SqlSoundingJobRepository,
    SqlSoundingRepository,
    SqlSoundingScheduleConfigRepository,
    SqlSoundingScheduleRepository,
    SqlStationRepository,
    SqlTokenRepository,
    SqlUserRepository,
)
from app.services.auth import AuthService
from app.services.devices import DeviceService
from app.services.errors import ErrorService
from app.services.measurements import MeasurementService
from app.services.soundings import SoundingService
from app.services.stations import StationService
from app.services.users import UserService


class AppProvider(Provider):
    @provide(scope=Scope.APP)
    def provide_settings(self) -> Settings:
        return Settings()

    @provide(scope=Scope.APP)
    def provide_engine(self, settings: Settings) -> AsyncEngine:
        return create_engine(settings)

    @provide(scope=Scope.APP)
    def provide_session_factory(self, engine: AsyncEngine) -> async_sessionmaker[AsyncSession]:
        return create_session_factory(engine)

    @provide(scope=Scope.APP)
    async def provide_redis(self, settings: Settings) -> AsyncIterator[ArqRedis]:
        pool = await create_pool(RedisSettings.from_dsn(settings.redis_url))
        try:
            yield pool
        finally:
            await pool.close()

    @provide(scope=Scope.REQUEST)
    async def provide_session(
        self, session_factory: async_sessionmaker[AsyncSession]
    ) -> AsyncIterator[AsyncSession]:
        async with session_factory() as session:
            try:
                yield session
                await session.commit()
            except Exception:
                await session.rollback()
                raise

    @provide(scope=Scope.REQUEST)
    def provide_device_repo(self, session: AsyncSession) -> DeviceRepository:
        return SqlDeviceRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_measurement_repo(self, session: AsyncSession) -> MeasurementRepository:
        return SqlMeasurementRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_station_repo(self, session: AsyncSession) -> StationRepository:
        return SqlStationRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_repo(self, session: AsyncSession) -> SoundingRepository:
        return SqlSoundingRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_job_repo(self, session: AsyncSession) -> SoundingJobRepository:
        return SqlSoundingJobRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_export_job_repo(self, session: AsyncSession) -> SoundingExportJobRepository:
        return SqlSoundingExportJobRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_schedule_repo(self, session: AsyncSession) -> SoundingScheduleRepository:
        return SqlSoundingScheduleRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_schedule_config_repo(self, session: AsyncSession) -> SoundingScheduleConfigRepository:
        return SqlSoundingScheduleConfigRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_error_repo(self, session: AsyncSession) -> ErrorRepository:
        return SqlErrorRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_user_repo(self, session: AsyncSession) -> UserRepository:
        return SqlUserRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_token_repo(self, session: AsyncSession) -> TokenRepository:
        return SqlTokenRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_auth_service(self, users: UserRepository, tokens: TokenRepository, settings: Settings) -> AuthService:
        return AuthService(users, tokens, settings)

    @provide(scope=Scope.REQUEST)
    def provide_device_service(self, devices: DeviceRepository) -> DeviceService:
        return DeviceService(devices)

    @provide(scope=Scope.REQUEST)
    def provide_measurement_service(self, measurements: MeasurementRepository) -> MeasurementService:
        return MeasurementService(measurements)

    @provide(scope=Scope.REQUEST)
    def provide_station_service(self, stations: StationRepository, settings: Settings) -> StationService:
        return StationService(stations, settings)

    @provide(scope=Scope.REQUEST)
    def provide_sounding_service(
        self,
        soundings: SoundingRepository,
        jobs: SoundingJobRepository,
        export_jobs: SoundingExportJobRepository,
        schedules: SoundingScheduleRepository,
        schedule_config: SoundingScheduleConfigRepository,
        stations: StationRepository,
        settings: Settings,
        redis: ArqRedis,
    ) -> SoundingService:
        return SoundingService(soundings, jobs, export_jobs, schedules, schedule_config, stations, settings, redis)

    @provide(scope=Scope.REQUEST)
    def provide_error_service(self, errors: ErrorRepository) -> ErrorService:
        return ErrorService(errors)

    @provide(scope=Scope.REQUEST)
    def provide_user_service(self, users: UserRepository) -> UserService:
        return UserService(users)
