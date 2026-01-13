from __future__ import annotations

from dishka import Provider, Scope, provide
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import Settings
from app.db.session import create_engine, create_session_factory
from app.repositories.interfaces import DeviceRepository, MeasurementRepository, TokenRepository, UserRepository
from app.repositories.sqlalchemy import SqlDeviceRepository, SqlMeasurementRepository, SqlTokenRepository, SqlUserRepository
from app.services.auth import AuthService
from app.services.devices import DeviceService
from app.services.measurements import MeasurementService
from app.services.users import UserService


class AppProvider(Provider):
    @provide(scope=Scope.APP)
    def provide_settings(self) -> Settings:
        return Settings()

    @provide(scope=Scope.APP)
    def provide_engine(self, settings: Settings):
        return create_engine(settings)

    @provide(scope=Scope.APP)
    def provide_session_factory(self, engine):
        return create_session_factory(engine)

    @provide(scope=Scope.REQUEST)
    async def provide_session(self, session_factory) -> AsyncSession:
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
    def provide_user_repo(self, session: AsyncSession) -> UserRepository:
        return SqlUserRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_token_repo(self, session: AsyncSession) -> TokenRepository:
        return SqlTokenRepository(session)

    @provide(scope=Scope.REQUEST)
    def provide_auth_service(self, users: UserRepository, tokens: TokenRepository) -> AuthService:
        return AuthService(users, tokens)

    @provide(scope=Scope.REQUEST)
    def provide_device_service(self, devices: DeviceRepository) -> DeviceService:
        return DeviceService(devices)

    @provide(scope=Scope.REQUEST)
    def provide_measurement_service(self, measurements: MeasurementRepository) -> MeasurementService:
        return MeasurementService(measurements)

    @provide(scope=Scope.REQUEST)
    def provide_user_service(self, users: UserRepository) -> UserService:
        return UserService(users)
