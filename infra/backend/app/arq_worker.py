from __future__ import annotations

import logging
from datetime import datetime, timezone
from typing import Any

from arq import cron
from arq.connections import RedisSettings
from dishka import make_async_container
from dishka.integrations.arq import FromDishka, inject, setup_dishka

from app.container import AppProvider
from app.core.config import Settings
from app.services.stations import StationService

logger = logging.getLogger(__name__)


async def _refresh_stations(stations: StationService, target_hour: int) -> None:
    now = datetime.now(timezone.utc)
    target_dt = datetime(now.year, now.month, now.day, target_hour, 0, 0, tzinfo=timezone.utc)
    updated, fetched = await stations.refresh_for_datetime(target_dt)
    logger.info("stations refresh target=%s updated=%s fetched=%s", target_dt.isoformat(), updated, fetched)


@inject
async def refresh_stations_midnight(
    ctx: dict[str, Any],
    stations: FromDishka[StationService],
) -> None:
    await _refresh_stations(stations, target_hour=0)


@inject
async def refresh_stations_noon(
    ctx: dict[str, Any],
    stations: FromDishka[StationService],
) -> None:
    await _refresh_stations(stations, target_hour=12)


settings = Settings()
container = make_async_container(AppProvider())


class WorkerSettings:
    redis_settings = RedisSettings.from_dsn(settings.redis_url)
    functions = [refresh_stations_midnight, refresh_stations_noon]
    cron_jobs = [
        cron(refresh_stations_midnight, hour=11, minute=0),
        cron(refresh_stations_noon, hour=23, minute=0),
    ]


setup_dishka(container=container, worker_settings=WorkerSettings)
