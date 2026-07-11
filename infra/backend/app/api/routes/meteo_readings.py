from __future__ import annotations

from datetime import datetime

from dishka.integrations.fastapi import FromDishka, inject
from fastapi import APIRouter, Depends, HTTPException, Query

from app.api.deps import get_current_user
from app.api.schemas import MeteoReadingPointOut, MeteoReadingsResponse
from app.domain.entities import User
from app.services.meteo_readings import MeteoReadingService


router = APIRouter(prefix="/meteo-readings", tags=["meteo-readings"])


def parse_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(raw)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail="Invalid ISO datetime") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise HTTPException(status_code=422, detail="Datetime timezone is required")
    return parsed


def validate_range(start: datetime | None, end: datetime | None) -> tuple[datetime, datetime]:
    if start is None or end is None:
        raise HTTPException(status_code=422, detail="Both from and to are required")
    if start > end:
        raise HTTPException(status_code=422, detail="from must be earlier than or equal to to")
    return start, end


@router.get("", response_model=MeteoReadingsResponse)
@inject
async def list_meteo_readings(
    readings: FromDishka[MeteoReadingService],
    device_id: str = Query(..., min_length=1, max_length=64),
    start: str = Query(..., alias="from"),
    end: str = Query(..., alias="to"),
    limit: int = Query(2000, ge=1, le=10000),
    bucket_seconds: int | None = Query(None, ge=0, le=86400),
    current_user: User = Depends(get_current_user),
) -> MeteoReadingsResponse:
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    start_dt, end_dt = validate_range(start_dt, end_dt)
    points, raw_count, bucket_value, bucket_label, aggregated = await readings.list_series(
        device_id=device_id,
        start=start_dt,
        end=end_dt,
        limit=limit,
        bucket_seconds=bucket_seconds if bucket_seconds and bucket_seconds > 0 else None,
    )
    return MeteoReadingsResponse(
        points=[MeteoReadingPointOut.model_validate(item, from_attributes=True) for item in points],
        raw_count=raw_count,
        limit=limit,
        bucket_seconds=bucket_value,
        bucket_label=bucket_label,
        aggregated=aggregated,
    )
