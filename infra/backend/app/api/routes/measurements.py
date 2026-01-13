from datetime import datetime

from fastapi import APIRouter, Depends, Query
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import MeasurementLatestResponse, MeasurementPointOut, MeasurementsResponse
from app.domain.entities import User
from app.services.measurements import MeasurementService

router = APIRouter(prefix="/measurements", tags=["measurements"])


def parse_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    return datetime.fromisoformat(raw)


@router.get("", response_model=MeasurementsResponse)
@inject
async def list_measurements(
    measurements: FromDishka[MeasurementService],
    device_id: str = Query(..., min_length=1),
    start: str | None = Query(None, alias="from"),
    end: str | None = Query(None, alias="to"),
    limit: int = Query(2000, ge=1, le=10000),
    bucket_seconds: int | None = Query(None, ge=0, le=86400),
    current_user: User = Depends(get_current_user),
):
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    points, raw_count, bucket_seconds, bucket_label, aggregated = await measurements.list_series(
        device_id=device_id,
        start=start_dt,
        end=end_dt,
        limit=limit,
        bucket_seconds=bucket_seconds if bucket_seconds and bucket_seconds > 0 else None,
    )
    return MeasurementsResponse(
        points=[MeasurementPointOut.model_validate(item, from_attributes=True) for item in points],
        raw_count=raw_count,
        limit=limit,
        bucket_seconds=bucket_seconds,
        bucket_label=bucket_label,
        aggregated=aggregated,
    )


@router.get("/last", response_model=MeasurementLatestResponse)
@inject
async def last_measurement(
    measurements: FromDishka[MeasurementService],
    device_id: str = Query(..., min_length=1),
    current_user: User = Depends(get_current_user),
):
    timestamp = await measurements.latest_timestamp(device_id=device_id)
    if not timestamp:
        return MeasurementLatestResponse(timestamp=None, timestamp_ms=None)
    return MeasurementLatestResponse(
        timestamp=timestamp,
        timestamp_ms=int(timestamp.timestamp() * 1000),
    )
