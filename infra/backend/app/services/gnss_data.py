from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Sequence

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
        dataset = await self._repository.get(device_id, gnss_data_id)
        if not dataset:
            raise LookupError("GNSS dataset not found")

        rows, parsed_rows, duplicate_rows, skipped_rows, errors = self._parse_rows(raw_text)
        if not rows:
            raise GnssDataImportError(errors or ["No valid GNSS rows found"])

        upserted_rows = 0
        for start in range(0, len(rows), DEFAULT_CHUNK_SIZE):
            upserted_rows += await self._repository.upsert_measurements(
                gnss_data_id, rows[start : start + DEFAULT_CHUNK_SIZE]
            )
        refreshed = await self._repository.refresh_stats(device_id, gnss_data_id)
        dataset = refreshed or dataset
        return GnssImportSummary(
            dataset=dataset,
            parsed_rows=parsed_rows,
            upserted_rows=upserted_rows,
            duplicate_rows=duplicate_rows,
            skipped_rows=skipped_rows,
            first_timestamp=rows[0]["measured_at"],  # type: ignore[index]
            last_timestamp=rows[-1]["measured_at"],  # type: ignore[index]
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
            line = raw_line.strip()
            if not line or line.startswith("%"):
                continue
            parts = line.split()
            if len(parts) != 9:
                skipped_rows += 1
                self._append_error(errors, line_no, "expected 9 whitespace-separated columns")
                continue
            try:
                year, month, day, hour, minute, second = (int(value) for value in parts[:6])
                measured_at = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
                pw_mm = self._finite_float(parts[6], required=True)
                spw_mm = self._finite_float(parts[7], required=False)
                temperature_c = self._finite_float(parts[8], required=False)
            except (TypeError, ValueError) as exc:
                skipped_rows += 1
                self._append_error(errors, line_no, str(exc))
                continue
            parsed_rows += 1
            if measured_at in by_time:
                duplicate_rows += 1
            by_time[measured_at] = {
                "measured_at": measured_at,
                "pw_mm": pw_mm,
                "spw_mm": spw_mm,
                "temperature_c": temperature_c,
            }

        rows = [by_time[key] for key in sorted(by_time)]
        return rows, parsed_rows, duplicate_rows, skipped_rows, errors

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
