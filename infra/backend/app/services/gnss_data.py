from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Iterable, Sequence

from app.domain.entities import GnssData, GnssDataMeasurementPoint
from app.repositories.interfaces import GnssDataRepository


DEFAULT_CHUNK_SIZE = 4000
MAX_IMPORT_ERRORS = 20


@dataclass
class GnssImportSummary:
    dataset: GnssData
    parsed_rows: int
    upserted_rows: int
    duplicate_rows: int
    skipped_rows: int
    first_timestamp: datetime | None
    last_timestamp: datetime | None
    errors: list[str] = field(default_factory=list)


@dataclass
class GnssDataSeries:
    dataset: GnssData
    points: Sequence[GnssDataMeasurementPoint]
    capped: bool


class GnssDataImportError(ValueError):
    def __init__(self, errors: list[str]) -> None:
        super().__init__("GNSS file has no valid rows")
        self.errors = errors


class GnssDataService:
    def __init__(self, repository: GnssDataRepository) -> None:
        self._repository = repository

    async def list(self, device_id: str) -> Sequence[GnssData]:
        return await self._repository.list(device_id)

    async def create(
        self,
        device_id: str,
        name: str,
        description: str | None = None,
    ) -> GnssData:
        return await self._repository.create(
            device_id=device_id,
            name=self._normalize_name(name),
            description=self._clean_optional(description),
        )

    async def update(
        self,
        device_id: str,
        gnss_data_id: str,
        name: str | None = None,
        description: str | None = None,
        description_set: bool = False,
    ) -> GnssData | None:
        return await self._repository.update(
            device_id=device_id,
            gnss_data_id=gnss_data_id,
            name=self._normalize_name(name) if name is not None else None,
            description=self._clean_optional(description) if description_set else None,
            description_set=description_set,
        )

    async def delete(self, device_id: str, gnss_data_id: str) -> bool:
        return await self._repository.delete(device_id, gnss_data_id)

    async def import_text(self, device_id: str, gnss_data_id: str, raw_text: str) -> GnssImportSummary:
        return await self.import_lines(device_id, gnss_data_id, raw_text.splitlines())

    async def import_lines(
        self, device_id: str, gnss_data_id: str, lines: Iterable[str]
    ) -> GnssImportSummary:
        dataset = await self._repository.get(device_id, gnss_data_id)
        if not dataset:
            raise LookupError("GNSS dataset not found")

        chunk: dict[datetime, dict[str, object]] = {}
        seen_times: set[datetime] = set()
        parsed_rows = 0
        duplicate_rows = 0
        skipped_rows = 0
        upserted_rows = 0
        first_timestamp: datetime | None = None
        last_timestamp: datetime | None = None
        errors: list[str] = []

        async def flush_chunk() -> None:
            nonlocal upserted_rows
            if not chunk:
                return
            rows = [chunk[key] for key in sorted(chunk)]
            upserted_rows += await self._repository.upsert_measurements(gnss_data_id, rows)
            await self._repository.commit()
            chunk.clear()

        for line_no, raw_line in enumerate(lines, start=1):
            parsed = self._parse_line(raw_line, line_no, errors)
            if parsed is None:
                if raw_line.strip() and not raw_line.strip().startswith("%"):
                    skipped_rows += 1
                continue
            parsed_rows += 1
            measured_at = parsed["measured_at"]  # type: ignore[assignment]
            if not isinstance(measured_at, datetime):
                skipped_rows += 1
                continue
            if measured_at in seen_times:
                duplicate_rows += 1
            seen_times.add(measured_at)
            first_timestamp = measured_at if first_timestamp is None else min(first_timestamp, measured_at)
            last_timestamp = measured_at if last_timestamp is None else max(last_timestamp, measured_at)
            chunk[measured_at] = parsed
            if len(chunk) >= DEFAULT_CHUNK_SIZE:
                await flush_chunk()

        if not parsed_rows:
            raise GnssDataImportError(errors or ["No valid GNSS rows found"])

        await flush_chunk()
        refreshed = await self._repository.refresh_stats(device_id, gnss_data_id)
        await self._repository.commit()
        dataset = refreshed or dataset
        return GnssImportSummary(
            dataset=dataset,
            parsed_rows=parsed_rows,
            upserted_rows=upserted_rows,
            duplicate_rows=duplicate_rows,
            skipped_rows=skipped_rows,
            first_timestamp=first_timestamp,
            last_timestamp=last_timestamp,
            errors=errors,
        )

    async def series(
        self,
        device_id: str,
        gnss_data_ids: Sequence[str],
        start: datetime,
        end: datetime,
        limit_per_dataset: int,
    ) -> Sequence[GnssDataSeries]:
        datasets = {item.id: item for item in await self._repository.list(device_id)}
        selected_ids = [item for item in gnss_data_ids if item in datasets]
        points_by_id = await self._repository.list_points(
            device_id=device_id,
            gnss_data_ids=selected_ids,
            start=start,
            end=end,
            limit_per_dataset=limit_per_dataset,
        )
        return [
            GnssDataSeries(
                dataset=datasets[dataset_id],
                points=points_by_id.get(dataset_id, []),
                capped=len(points_by_id.get(dataset_id, [])) >= limit_per_dataset,
            )
            for dataset_id in selected_ids
        ]

    def _parse_rows(self, raw_text: str) -> tuple[list[dict[str, object]], int, int, int, list[str]]:
        by_time: dict[datetime, dict[str, object]] = {}
        parsed_rows = 0
        duplicate_rows = 0
        skipped_rows = 0
        errors: list[str] = []

        for line_no, raw_line in enumerate(raw_text.splitlines(), start=1):
            parsed = self._parse_line(raw_line, line_no, errors)
            if parsed is None:
                if raw_line.strip() and not raw_line.strip().startswith("%"):
                    skipped_rows += 1
                continue
            parsed_rows += 1
            measured_at = parsed["measured_at"]
            if measured_at in by_time:
                duplicate_rows += 1
            by_time[measured_at] = parsed

        rows = [by_time[key] for key in sorted(by_time)]
        return rows, parsed_rows, duplicate_rows, skipped_rows, errors

    def _parse_line(self, raw_line: str, line_no: int, errors: list[str]) -> dict[str, object] | None:
        line = raw_line.strip()
        if not line or line.startswith("%"):
            return None
        parts = line.split()
        if len(parts) != 9:
            self._append_error(errors, line_no, "expected 9 whitespace-separated columns")
            return None
        try:
            year, month, day, hour, minute, second = (int(value) for value in parts[:6])
            measured_at = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
            pw_mm = self._finite_float(parts[6], required=True)
            spw_mm = self._finite_float(parts[7], required=False)
            temperature_c = self._finite_float(parts[8], required=False)
        except (TypeError, ValueError) as exc:
            self._append_error(errors, line_no, str(exc))
            return None
        return {
            "measured_at": measured_at,
            "pw_mm": pw_mm,
            "spw_mm": spw_mm,
            "temperature_c": temperature_c,
        }

    def _append_error(self, errors: list[str], line_no: int, message: str) -> None:
        if len(errors) < MAX_IMPORT_ERRORS:
            errors.append(f"Line {line_no}: {message}")

    def _finite_float(self, value: str, *, required: bool) -> float | None:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError(f"non-finite value {value!r}")
        if required and parsed is None:
            raise ValueError("missing required value")
        return parsed

    def _normalize_name(self, value: str) -> str:
        name = value.strip()
        if not name:
            raise ValueError("GNSS dataset name is required")
        if len(name) > 128:
            raise ValueError("GNSS dataset name is too long")
        return name

    def _clean_optional(self, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned or None
