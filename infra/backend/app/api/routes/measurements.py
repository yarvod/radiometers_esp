from __future__ import annotations

from datetime import datetime

from fastapi import APIRouter, Depends, Query
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import MeasurementOut
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


@router.get("", response_model=list[MeasurementOut])
@inject
async def list_measurements(
    device_id: str = Query(..., min_length=1),
    start: str | None = Query(None, alias="from"),
    end: str | None = Query(None, alias="to"),
    limit: int = Query(2000, ge=1, le=10000),
    current_user: User = Depends(get_current_user),
    measurements: FromDishka[MeasurementService],
):
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    result = await measurements.list(device_id=device_id, start=start_dt, end=end_dt, limit=limit)
    return [MeasurementOut.model_validate(item, from_attributes=True) for item in result]
