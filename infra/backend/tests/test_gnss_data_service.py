from __future__ import annotations

from datetime import datetime, timedelta, timezone

import pytest

from app.domain.entities import GnssData
from app.services.gnss_data import GnssDataImportError, GnssDataService


def make_dataset(count: int = 0) -> GnssData:
    now = datetime.now(timezone.utc)
    return GnssData(
        id="gnss-1",
        device_id="dev-1",
        name="External GNSS",
        description=None,
        measurement_count=count,
        start_at=None,
        end_at=None,
        last_import_at=None,
        created_at=now,
        updated_at=now,
    )


class FakeGnssRepository:
    def __init__(self) -> None:
        self.dataset = make_dataset()
        self.rows: list[dict[str, object]] = []
        self.batch_sizes: list[int] = []
        self.commits = 0

    async def list(self, device_id: str):
        return [self.dataset]

    async def get(self, device_id: str, gnss_data_id: str):
        return self.dataset if device_id == "dev-1" and gnss_data_id == "gnss-1" else None

    async def create(self, *args, **kwargs):
        return self.dataset

    async def update(self, *args, **kwargs):
        return self.dataset

    async def delete(self, *args, **kwargs):
        return True

    async def upsert_measurements(self, gnss_data_id: str, rows):
        self.batch_sizes.append(len(rows))
        self.rows.extend(rows)
        return len(rows)

    async def refresh_stats(self, device_id: str, gnss_data_id: str):
        self.dataset.measurement_count = len(self.rows)
        self.dataset.start_at = self.rows[0]["measured_at"]
        self.dataset.end_at = self.rows[-1]["measured_at"]
        return self.dataset

    async def commit(self):
        self.commits += 1

    async def list_points(self, *args, **kwargs):
        return {}


@pytest.mark.asyncio
async def test_import_text_skips_header_and_blank_lines():
    repo = FakeGnssRepository()
    service = GnssDataService(repo)  # type: ignore[arg-type]

    summary = await service.import_text(
        "dev-1",
        "gnss-1",
        """
% yy mn dd hh mm ss PW(mm) sPW(mm) T(C)
2026 05 17 10 00 00 6.47 5.22 5.25

2026 05 17 11 00 00 6.48 4.66 5.30
""",
    )

    assert summary.parsed_rows == 2
    assert summary.upserted_rows == 2
    assert summary.skipped_rows == 0
    assert repo.rows[0]["pw_mm"] == 6.47
    assert repo.rows[1]["temperature_c"] == 5.30


@pytest.mark.asyncio
async def test_import_text_deduplicates_timestamp_last_row_wins():
    repo = FakeGnssRepository()
    service = GnssDataService(repo)  # type: ignore[arg-type]

    summary = await service.import_text(
        "dev-1",
        "gnss-1",
        """
2026 05 17 10 00 00 6.47 5.22 5.25
2026 05 17 10 00 00 7.10 4.10 6.00
""",
    )

    assert summary.parsed_rows == 2
    assert summary.duplicate_rows == 1
    assert summary.upserted_rows == 1
    assert repo.rows[0]["pw_mm"] == 7.10
    assert repo.rows[0]["spw_mm"] == 4.10


@pytest.mark.asyncio
async def test_import_text_rejects_file_without_valid_rows():
    repo = FakeGnssRepository()
    service = GnssDataService(repo)  # type: ignore[arg-type]

    with pytest.raises(GnssDataImportError) as exc:
        await service.import_text("dev-1", "gnss-1", "bad row\n")

    assert exc.value.errors
    assert repo.rows == []


@pytest.mark.asyncio
async def test_import_text_chunks_large_files_under_asyncpg_parameter_limit():
    repo = FakeGnssRepository()
    service = GnssDataService(repo)  # type: ignore[arg-type]
    start = datetime(2023, 1, 1, tzinfo=timezone.utc)
    rows = []
    for idx in range(4001):
        dt = start + timedelta(hours=idx)
        rows.append(
            f"{dt.year:04d} {dt.month:02d} {dt.day:02d} {dt.hour:02d} {dt.minute:02d} {dt.second:02d} 6.47 5.22 5.25"
        )

    summary = await service.import_text("dev-1", "gnss-1", "\n".join(rows))

    assert summary.upserted_rows == 4001
    assert repo.batch_sizes == [4000, 1]
    assert repo.commits == 3
