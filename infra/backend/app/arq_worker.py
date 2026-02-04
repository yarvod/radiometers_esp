from __future__ import annotations

import logging
from datetime import datetime, timezone
from typing import Any

from arq import cron
from arq.connections import RedisSettings
from dishka import make_async_container

from app.container import AppProvider
from app.core.config import Settings
from app.services.stations import StationService

logger = logging.getLogger(__name__)


async def startup(ctx: dict[str, Any]) -> None:
    ctx["container"] = make_async_container(AppProvider())


async def shutdown(ctx: dict[str, Any]) -> None:
    container = ctx.get("container")
    if container:
        await container.close()


async def refresh_stations_job(ctx: dict[str, Any], target_hour: int) -> None:
    container = ctx["container"]
    now = datetime.now(timezone.utc)
    target_dt = datetime(now.year, now.month, now.day, target_hour, 0, 0, tzinfo=timezone.utc)
    async with container() as request_container:
        stations = await request_container.get(StationService)
        updated, fetched = await stations.refresh_for_datetime(target_dt)
    logger.info("stations refresh target=%s updated=%s fetched=%s", target_dt.isoformat(), updated, fetched)


settings = Settings()


class WorkerSettings:
    redis_settings = RedisSettings.from_dsn(settings.redis_url)
    functions = [refresh_stations_job]
    on_startup = startup
    on_shutdown = shutdown
    cron_jobs = [
        cron(refresh_stations_job, hour=11, minute=0, kwargs={"target_hour": 0}),
        cron(refresh_stations_job, hour=23, minute=0, kwargs={"target_hour": 12}),
    ]
