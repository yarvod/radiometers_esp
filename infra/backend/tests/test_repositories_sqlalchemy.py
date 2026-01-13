from datetime import datetime, timezone
from unittest.mock import AsyncMock, Mock

import pytest

from app.domain.entities import Measurement
from app.db.models import AccessTokenModel, DeviceModel, MeasurementModel
from app.repositories.sqlalchemy import (
    SqlDeviceRepository,
    SqlMeasurementRepository,
    SqlTokenRepository,
)


class FakeScalars:
    def __init__(self, values):
        self._values = values

    def all(self):
        return self._values


class FakeResult:
    def __init__(self, scalar=None, scalars=None):
        self._scalar = scalar
        self._scalars = scalars or []

    def scalar_one_or_none(self):
        return self._scalar

    def scalars(self):
        return FakeScalars(self._scalars)


@pytest.mark.asyncio
async def test_token_repo_create_adds_and_flushes():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()

    repo = SqlTokenRepository(session)
    expires_at = datetime.now(timezone.utc)
    token = await repo.create("token-1", "user-1", expires_at)

    assert token.token == "token-1"
    assert token.user_id == "user-1"
    assert token.expires_at == expires_at
    session.add.assert_called_once()
    assert isinstance(session.add.call_args.args[0], AccessTokenModel)
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_token_repo_get_returns_none():
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalar=None))

    repo = SqlTokenRepository(session)
    result = await repo.get("missing")

    assert result is None
    session.execute.assert_awaited_once()


@pytest.mark.asyncio
async def test_token_repo_delete_executes():
    session = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult())

    repo = SqlTokenRepository(session)
    await repo.delete("token-1")

    session.execute.assert_awaited_once()


@pytest.mark.asyncio
async def test_device_repo_touch_updates_existing():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()

    model = DeviceModel(id="dev1", display_name=None, last_seen_at=None)
    session.execute = AsyncMock(return_value=FakeResult(scalar=model))

    repo = SqlDeviceRepository(session)
    now = datetime.now(timezone.utc)
    device = await repo.touch("dev1", now)

    assert device.last_seen_at == now
    session.add.assert_not_called()
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_device_repo_touch_creates_new():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()
    session.execute = AsyncMock(return_value=FakeResult(scalar=None))

    repo = SqlDeviceRepository(session)
    now = datetime.now(timezone.utc)
    device = await repo.touch("dev2", now)

    assert device.id == "dev2"
    session.add.assert_called_once()
    assert isinstance(session.add.call_args.args[0], DeviceModel)
    session.flush.assert_awaited_once()


@pytest.mark.asyncio
async def test_measurement_repo_add_flushes():
    session = AsyncMock()
    session.add = Mock()
    session.flush = AsyncMock()

    repo = SqlMeasurementRepository(session)
    measurement = Measurement(
        id="m1",
        device_id="dev1",
        timestamp=datetime.now(timezone.utc),
        timestamp_ms=1234,
        adc1=1.0,
        adc2=2.0,
        adc3=3.0,
        temps=[10.0, 11.0],
        bus_v=5.0,
        bus_i=0.1,
        bus_p=0.5,
        adc1_cal=None,
        adc2_cal=None,
        adc3_cal=None,
        log_use_motor=False,
        log_duration=1.0,
        log_filename=None,
    )

    await repo.add(measurement)

    session.add.assert_called_once()
    assert isinstance(session.add.call_args.args[0], MeasurementModel)
    session.flush.assert_awaited_once()
