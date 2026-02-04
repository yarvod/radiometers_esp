from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone

import httpx

from app.core.config import Settings
from app.domain.entities import Station
from app.repositories.interfaces import StationRepository

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class StationPayload:
    station_id: str
    name: str | None
    lat: float | None
    lon: float | None
    src: str | None


def _parse_float(value: object) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        raw = value.strip()
        if not raw:
            return None
        try:
            return float(raw)
        except ValueError:
            return None
    return None


def _parse_station(raw: dict) -> StationPayload | None:
    station_id = str(raw.get("stationid", "")).strip()
    if not station_id:
        return None
    name_raw = raw.get("name")
    name = str(name_raw).strip() if name_raw is not None else None
    if name == "":
        name = None
    src_raw = raw.get("src")
    src = str(src_raw).strip() if src_raw is not None else None
    if src == "":
        src = None
    return StationPayload(
        station_id=station_id,
        name=name,
        lat=_parse_float(raw.get("lat")),
        lon=_parse_float(raw.get("lon")),
        src=src,
    )


class StationService:
    def __init__(self, stations: StationRepository, settings: Settings) -> None:
        self._stations = stations
        self._settings = settings

    async def list(self, limit: int, offset: int, query: str | None) -> list[Station]:
        query = query.strip() if query else None
        return list(await self._stations.list(limit=limit, offset=offset, query=query))

    async def count(self, query: str | None) -> int:
        query = query.strip() if query else None
        return await self._stations.count(query=query)

    async def get(self, station_id: str) -> Station | None:
        return await self._stations.get(station_id.strip())

    async def update(
        self,
        station_id: str,
        name: str | None,
        lat: float | None,
        lon: float | None,
        src: str | None,
    ) -> Station:
        station_id = station_id.strip()
        updated_at = datetime.now(timezone.utc)
        return await self._stations.upsert(
            station_id=station_id,
            name=name,
            lat=lat,
            lon=lon,
            src=src,
            updated_at=updated_at,
        )

    async def refresh_for_datetime(self, dt: datetime) -> tuple[int, int]:
        stations = await self._fetch_stations(dt)
        fetched_at = datetime.now(timezone.utc)
        updated = 0
        for item in stations:
            await self._stations.upsert(
                station_id=item.station_id,
                name=item.name,
                lat=item.lat,
                lon=item.lon,
                src=item.src,
                updated_at=fetched_at,
            )
            updated += 1
        return updated, len(stations)

    async def refresh_one(self, station_id: str, dt: datetime) -> Station | None:
        station_id = station_id.strip()
        stations = await self._fetch_stations(dt)
        fetched_at = datetime.now(timezone.utc)
        for item in stations:
            if item.station_id == station_id:
                return await self._stations.upsert(
                    station_id=item.station_id,
                    name=item.name,
                    lat=item.lat,
                    lon=item.lon,
                    src=item.src,
                    updated_at=fetched_at,
                )
        return None

    async def _fetch_stations(self, dt: datetime) -> list[StationPayload]:
        params = {"datetime": dt.strftime("%Y-%m-%d %H:%M:%S")}
        timeout = httpx.Timeout(
            self._settings.stations_request_timeout,
            connect=self._settings.stations_connect_timeout,
            read=self._settings.stations_read_timeout,
        )
        headers = {"User-Agent": self._settings.stations_user_agent}
        async with httpx.AsyncClient(timeout=timeout) as client:
            resp = await client.get(self._settings.stations_url, params=params, headers=headers)
        if resp.status_code != 200:
            raise RuntimeError(f"HTTP {resp.status_code}")
        logger.info("stations raw response: %s", resp.text)
        payload = resp.json()
        if not isinstance(payload, dict):
            raise RuntimeError("Invalid stations payload")
        raw_list = payload.get("stations", [])
        stations: list[StationPayload] = []
        if isinstance(raw_list, list):
            for raw in raw_list:
                if not isinstance(raw, dict):
                    continue
                parsed = _parse_station(raw)
                if parsed:
                    stations.append(parsed)
        logger.info("stations parsed count=%s", len(stations))
        return stations
