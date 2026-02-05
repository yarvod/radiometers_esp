from __future__ import annotations

import asyncio
import csv
import logging
import math
import re
import zipfile
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from io import StringIO
from pathlib import Path
from typing import Optional

import httpx
from arq.connections import ArqRedis
from bs4 import BeautifulSoup

from app.core.config import Settings
from app.domain.entities import (
    Sounding,
    SoundingExportJob,
    SoundingJob,
    SoundingScheduleConfig,
    SoundingScheduleItem,
)
from app.repositories.interfaces import (
    SoundingExportJobRepository,
    SoundingJobRepository,
    SoundingRepository,
    SoundingScheduleConfigRepository,
    SoundingScheduleRepository,
    StationRepository,
)

logger = logging.getLogger(__name__)

_COLUMN_UNITS = {
    "PRES": "hPa",
    "HGHT": "m",
    "TEMP": "C",
    "DWPT": "C",
    "RELH": "%",
    "MIXR": "g/kg",
    "DRCT": "deg",
    "SKNT": "knot",
    "THTA": "K",
    "THTE": "K",
    "THTV": "K",
    "ABSH": "g/m3",
}


@dataclass(frozen=True)
class SoundingPayload:
    station_name: str
    columns: list[str]
    rows: list[list[object]]
    units: dict[str, str]
    raw_text: str
    row_count: int


def _compute_absh(relh: Optional[float], temp_c: Optional[float]) -> Optional[float]:
    if relh is None or temp_c is None:
        return None
    try:
        return 6.112 * math.exp(17.67 * temp_c / (temp_c + 243.5)) * relh * 2.1674 / (273.15 + temp_c)
    except Exception:
        return None


def _is_number(text: str) -> bool:
    try:
        float(text)
        return True
    except Exception:
        return False


def _rows_to_csv(columns: list[str], rows: list[list[object]]) -> str:
    buf = StringIO()
    writer = csv.writer(buf, delimiter=";", lineterminator="\n")
    writer.writerow(columns)
    for row in rows:
        writer.writerow([value if value is not None else "" for value in row])
    return buf.getvalue()


def _slug_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]+", "_", value.strip())
    cleaned = cleaned.strip("_")
    return cleaned or "station"


def _csv_filename(station_label: str, dt: datetime) -> str:
    safe = _slug_name(station_label)
    utc_dt = dt.astimezone(timezone.utc)
    return f"{safe}_{utc_dt:%Y_%m_%d_%H}.csv"


def _parse_sounding(text_block: str) -> SoundingPayload:
    lines = text_block.splitlines()
    columns_raw: list[str] = []
    rows_raw: list[dict[str, object]] = []
    header_seen = False

    for line in lines:
        stripped = line.strip()
        if not stripped:
            if header_seen:
                break
            continue
        if not header_seen and stripped.startswith("PRES"):
            columns_raw = stripped.split()
            header_seen = True
            continue
        if header_seen:
            parts = stripped.split()
            if len(parts) < len(columns_raw):
                continue
            if not _is_number(parts[0]):
                continue
            row: dict[str, object] = {}
            for key, value in zip(columns_raw, parts):
                try:
                    row[key] = float(value)
                except ValueError:
                    row[key] = value
            rows_raw.append(row)

    if "RELH" in columns_raw and "TEMP" in columns_raw:
        columns_raw.append("ABSH")
        for row in rows_raw:
            relh = row.get("RELH")
            temp = row.get("TEMP")
            absh = _compute_absh(
                relh if isinstance(relh, (int, float)) else None,
                temp if isinstance(temp, (int, float)) else None,
            )
            row["ABSH"] = absh

    label_map: dict[str, str] = {}
    for base in columns_raw:
        unit = _COLUMN_UNITS.get(base)
        label_map[base] = f"{base},{unit}" if unit else base
    columns_labeled = [label_map[c] for c in columns_raw]

    rows_labeled: list[list[object]] = []
    for row in rows_raw:
        rows_labeled.append([row.get(base) for base in columns_raw])

    return SoundingPayload(
        station_name="",
        columns=columns_labeled,
        rows=rows_labeled,
        units=_COLUMN_UNITS,
        raw_text=text_block,
        row_count=len(rows_labeled),
    )


class SoundingService:
    def __init__(
        self,
        soundings: SoundingRepository,
        jobs: SoundingJobRepository,
        export_jobs: SoundingExportJobRepository,
        schedules: SoundingScheduleRepository,
        schedule_config: SoundingScheduleConfigRepository,
        stations: StationRepository,
        settings: Settings,
        redis: ArqRedis,
    ) -> None:
        self._soundings = soundings
        self._jobs = jobs
        self._export_jobs = export_jobs
        self._schedules = schedules
        self._schedule_config = schedule_config
        self._stations = stations
        self._settings = settings
        self._redis = redis

    async def list_soundings(
        self,
        station_code: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        offset: int,
    ) -> list[Sounding]:
        station = await self._stations.get(station_code.strip())
        if not station:
            return []
        return list(await self._soundings.list(station.id, start, end, limit, offset))

    async def count_soundings(self, station_code: str, start: datetime | None, end: datetime | None) -> int:
        station = await self._stations.get(station_code.strip())
        if not station:
            return 0
        return await self._soundings.count(station.id, start, end)

    async def get_sounding(self, sounding_id: str) -> Sounding | None:
        return await self._soundings.get(sounding_id)

    async def create_job(
        self,
        station_code: str,
        start_at: datetime,
        end_at: datetime,
        step_hours: int,
    ) -> SoundingJob:
        station = await self._stations.get(station_code.strip())
        if not station:
            raise ValueError("Station not found")
        if step_hours <= 0:
            raise ValueError("step_hours must be positive")
        if end_at < start_at:
            raise ValueError("end_at must be after start_at")
        job = await self._jobs.create(station.id, start_at, end_at, step_hours)
        await self._redis.enqueue_job("load_soundings_job", job.id)
        return job

    async def get_job(self, job_id: str) -> SoundingJob | None:
        return await self._jobs.get(job_id)

    async def process_job(self, job_id: str) -> None:
        job = await self._jobs.get(job_id)
        if not job:
            logger.warning("sounding job not found: %s", job_id)
            return
        await self._jobs.update_progress(job_id, status="running", total=None, done=None, error=None)
        times = self._build_times(job.start_at, job.end_at, job.step_hours)
        await self._jobs.update_progress(job_id, status=None, total=len(times), done=0, error=None)

        station = await self._stations.get_by_internal_id(job.station_id)
        if not station:
            await self._jobs.update_progress(job_id, status="failed", total=None, done=None, error="Station not found")
            return

        if not times:
            await self._jobs.update_progress(job_id, status="done", total=0, done=0, error=None)
            return

        done = 0
        last_error: str | None = None
        semaphore = asyncio.Semaphore(max(1, self._settings.soundings_concurrency))

        async def handle(dt: datetime, client: httpx.AsyncClient) -> tuple[datetime, SoundingPayload | None, Exception | None]:
            async with semaphore:
                try:
                    payload = await self._fetch_sounding(station.station_id, dt, client=client)
                    return dt, payload, None
                except Exception as exc:
                    return dt, None, exc

        async with self._build_client() as client:
            tasks = [asyncio.create_task(handle(dt, client)) for dt in times]
            for task in asyncio.as_completed(tasks):
                dt, payload, exc = await task
                if exc:
                    last_error = str(exc)
                    logger.exception("sounding fetch failed station=%s dt=%s", station.station_id, dt.isoformat())
                if payload:
                    await self._soundings.upsert(
                        station_id=station.id,
                        sounding_time=dt,
                        station_name=payload.station_name,
                        columns=payload.columns,
                        rows=payload.rows,
                        units=payload.units,
                        raw_text=payload.raw_text,
                        row_count=payload.row_count,
                    )
                done += 1
                await self._jobs.update_progress(job_id, status=None, total=None, done=done, error=last_error)

        await self._jobs.update_progress(job_id, status="done", total=None, done=done, error=last_error)

    async def create_export_job(self, station_code: str, sounding_ids: list[str]) -> SoundingExportJob:
        station = await self._stations.get(station_code.strip())
        if not station:
            raise ValueError("Station not found")
        ids = [sid.strip() for sid in sounding_ids if sid.strip()]
        if not ids:
            raise ValueError("No soundings selected")
        soundings = await self._soundings.get_many(ids)
        if not soundings:
            raise ValueError("Soundings not found")
        filtered = [s for s in soundings if s.station_id == station.id]
        if len(filtered) != len(set(ids)):
            raise ValueError("Some soundings not found or belong to another station")
        job = await self._export_jobs.create(station.id, [s.id for s in filtered])
        await self._redis.enqueue_job("export_soundings_job", job.id)
        return job

    async def get_export_job(self, job_id: str) -> SoundingExportJob | None:
        return await self._export_jobs.get(job_id)

    async def compute_pwv(
        self,
        station_code: str,
        sounding_ids: list[str],
        min_height: float,
    ) -> list[tuple[str, datetime, float | None]]:
        station = await self._stations.get(station_code.strip())
        if not station:
            raise ValueError("Station not found")
        ids = [sid.strip() for sid in sounding_ids if sid.strip()]
        if not ids:
            raise ValueError("No soundings selected")
        soundings = await self._soundings.get_many(ids)
        soundings = [s for s in soundings if s.station_id == station.id]
        items: list[tuple[str, datetime, float | None]] = []
        for sounding in soundings:
            pwv = self._compute_pwv_for_sounding(sounding, min_height)
            items.append((sounding.id, sounding.sounding_time, pwv))
        items.sort(key=lambda item: item[1])
        return items

    async def process_export_job(self, job_id: str) -> None:
        job = await self._export_jobs.get(job_id)
        if not job:
            logger.warning("export job not found: %s", job_id)
            return

        await self._export_jobs.update_progress(
            job_id,
            status="running",
            total=None,
            done=None,
            error=None,
            file_path=None,
            file_name=None,
        )
        soundings = await self._soundings.get_many(job.sounding_ids)
        soundings = [s for s in soundings if s.station_id == job.station_id]
        if not soundings:
            await self._export_jobs.update_progress(
                job_id,
                status="failed",
                total=0,
                done=0,
                error="Soundings not found",
                file_path=None,
                file_name=None,
            )
            return

        soundings.sort(key=lambda item: item.sounding_time)
        total = len(soundings)
        await self._export_jobs.update_progress(
            job_id,
            status=None,
            total=total,
            done=0,
            error=None,
            file_path=None,
            file_name=None,
        )

        station = await self._stations.get_by_internal_id(job.station_id)
        station_label = (station.name if station and station.name else station.station_id if station else "station")
        export_root = Path(self._settings.soundings_export_dir)
        export_dir = export_root / job.id
        export_dir.mkdir(parents=True, exist_ok=True)

        done = 0
        try:
            if total == 1:
                sounding = soundings[0]
                label = sounding.station_name or station_label
                file_name = _csv_filename(label, sounding.sounding_time)
                file_path = export_dir / file_name
                csv_text = _rows_to_csv(sounding.columns, sounding.rows)
                file_path.write_text(csv_text, encoding="utf-8")
                done = 1
                await self._export_jobs.update_progress(
                    job_id,
                    status="done",
                    total=total,
                    done=done,
                    error=None,
                    file_path=str(file_path),
                    file_name=file_name,
                )
                return

            zip_name = f"{_slug_name(station_label)}_{job.id[:8]}.zip"
            zip_path = export_dir / zip_name
            with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for sounding in soundings:
                    label = sounding.station_name or station_label
                    file_name = _csv_filename(label, sounding.sounding_time)
                    csv_text = _rows_to_csv(sounding.columns, sounding.rows)
                    archive.writestr(file_name, csv_text)
                    done += 1
                    await self._export_jobs.update_progress(
                        job_id,
                        status=None,
                        total=None,
                        done=done,
                        error=None,
                        file_path=None,
                        file_name=None,
                    )
            await self._export_jobs.update_progress(
                job_id,
                status="done",
                total=total,
                done=done,
                error=None,
                file_path=str(zip_path),
                file_name=zip_name,
            )
        except Exception as exc:
            await self._export_jobs.update_progress(
                job_id,
                status="failed",
                total=total,
                done=done,
                error=str(exc),
                file_path=None,
                file_name=None,
            )

    async def list_schedule(self) -> list[SoundingScheduleItem]:
        return list(await self._schedules.list())

    async def add_schedule(self, station_code: str) -> SoundingScheduleItem:
        station = await self._stations.get(station_code.strip())
        if not station:
            raise ValueError("Station not found")
        return await self._schedules.add(station.id)

    async def set_schedule_enabled(self, schedule_id: str, enabled: bool) -> SoundingScheduleItem:
        return await self._schedules.set_enabled(schedule_id, enabled)

    async def delete_schedule(self, schedule_id: str) -> None:
        await self._schedules.delete(schedule_id)

    async def get_schedule_config(self) -> SoundingScheduleConfig:
        return await self._schedule_config.get()

    async def update_schedule_config(self, interval_hours: int | None, offset_hours: int | None) -> SoundingScheduleConfig:
        return await self._schedule_config.update(interval_hours=interval_hours, offset_hours=offset_hours)

    async def run_schedule_tick(self) -> None:
        config = await self._schedule_config.get()
        now = datetime.now(timezone.utc)
        if config.interval_hours <= 0:
            return
        diff = now.hour - config.offset_hours
        if diff % config.interval_hours != 0:
            return
        target_hour = diff % 24
        target_dt = datetime(now.year, now.month, now.day, target_hour, 0, 0, tzinfo=timezone.utc)
        if target_hour > now.hour:
            target_dt = target_dt - timedelta(days=1)

        schedules = await self._schedules.list()
        for schedule in schedules:
            if not schedule.enabled:
                continue
            try:
                payload = await self._fetch_sounding(schedule.station_code, target_dt)
                if payload:
                    await self._soundings.upsert(
                        station_id=schedule.station_id,
                        sounding_time=target_dt,
                        station_name=payload.station_name,
                        columns=payload.columns,
                        rows=payload.rows,
                        units=payload.units,
                        raw_text=payload.raw_text,
                        row_count=payload.row_count,
                    )
            except Exception:
                logger.exception(
                    "scheduled sounding failed station=%s dt=%s",
                    schedule.station_code,
                    target_dt.isoformat(),
                )

    def _build_times(self, start_at: datetime, end_at: datetime, step_hours: int) -> list[datetime]:
        result: list[datetime] = []
        current = start_at
        while current <= end_at:
            result.append(current)
            current = current + timedelta(hours=step_hours)
        return result

    def _build_client(self) -> httpx.AsyncClient:
        timeout = httpx.Timeout(
            self._settings.soundings_request_timeout,
            connect=self._settings.soundings_connect_timeout,
            read=self._settings.soundings_read_timeout,
        )
        headers = {"User-Agent": self._settings.stations_user_agent}
        limits = httpx.Limits(
            max_connections=max(2, self._settings.soundings_concurrency * 2),
            max_keepalive_connections=max(2, self._settings.soundings_concurrency),
        )
        return httpx.AsyncClient(timeout=timeout, headers=headers, limits=limits)

    async def _fetch_sounding(
        self,
        station_code: str,
        dt: datetime,
        client: httpx.AsyncClient | None = None,
    ) -> SoundingPayload | None:
        params = {
            "datetime": dt.strftime("%Y-%m-%d %H:%M:%S"),
            "id": str(station_code),
            "type": "TEXT:LIST",
        }
        if client is None:
            async with self._build_client() as local_client:
                resp = await local_client.get(self._settings.soundings_url, params=params)
        else:
            resp = await client.get(self._settings.soundings_url, params=params)
        if resp.status_code == 404:
            return None
        if resp.status_code != 200:
            raise RuntimeError(f"HTTP {resp.status_code}")

        soup = BeautifulSoup(resp.text, "html.parser")
        h3 = soup.find("h3")
        station_name = h3.get_text(strip=True) if h3 else str(station_code)
        pre = soup.find("pre")
        if pre is None:
            return None
        text_block = pre.get_text("\n", strip=False)
        payload = _parse_sounding(text_block)
        return SoundingPayload(
            station_name=station_name,
            columns=payload.columns,
            rows=payload.rows,
            units=payload.units,
            raw_text=payload.raw_text,
            row_count=payload.row_count,
        )

    @staticmethod
    def _compute_pwv_for_sounding(sounding: Sounding, min_height: float) -> float | None:
        columns = sounding.columns or []
        rows = sounding.rows or []
        if not columns or not rows:
            return None
        col_map = {str(col).split(",", 1)[0].strip(): idx for idx, col in enumerate(columns)}
        h_idx = col_map.get("HGHT")
        absh_idx = col_map.get("ABSH")
        if h_idx is None or absh_idx is None:
            return None
        samples: list[tuple[float, float]] = []
        for row in rows:
            try:
                h_val = float(row[h_idx])
                a_val = float(row[absh_idx])
            except (TypeError, ValueError, IndexError):
                continue
            if h_val < min_height:
                continue
            samples.append((h_val, a_val))
        if len(samples) < 2:
            return None
        samples.sort(key=lambda x: x[0])
        total = 0.0
        prev_h, prev_a = samples[0]
        for h_val, a_val in samples[1:]:
            dh = h_val - prev_h
            if dh <= 0:
                prev_h, prev_a = h_val, a_val
                continue
            total += (prev_a + a_val) * 0.5 * dh
            prev_h, prev_a = h_val, a_val
        return total / 1000.0
