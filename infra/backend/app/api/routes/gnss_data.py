from datetime import datetime, timezone

from fastapi import APIRouter, Depends, File, HTTPException, Query, UploadFile, status
from sqlalchemy.exc import IntegrityError
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import (
    GnssDataCreateRequest,
    GnssDataImportResponse,
    GnssDataMeasurementPointOut,
    GnssDataOut,
    GnssDataSeriesOut,
    GnssDataSeriesResponse,
    GnssDataUpdateRequest,
)
from app.domain.entities import User
from app.services.gnss_data import GnssDataImportError, GnssDataService


router = APIRouter(prefix="/devices/{device_id}/gnss-data", tags=["gnss-data"])


def parse_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    parsed = datetime.fromisoformat(raw)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


@router.get("", response_model=list[GnssDataOut])
@inject
async def list_gnss_data(
    device_id: str,
    gnss_data: FromDishka[GnssDataService],
    current_user: User = Depends(get_current_user),
):
    items = await gnss_data.list(device_id)
    return [GnssDataOut.model_validate(item, from_attributes=True) for item in items]


@router.post("", response_model=GnssDataOut)
@inject
async def create_gnss_data(
    device_id: str,
    payload: GnssDataCreateRequest,
    gnss_data: FromDishka[GnssDataService],
    current_user: User = Depends(get_current_user),
):
    try:
        item = await gnss_data.create(
            device_id=device_id,
            name=payload.name,
            description=payload.description,
        )
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    except IntegrityError as exc:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="GNSS dataset name already exists") from exc
    return GnssDataOut.model_validate(item, from_attributes=True)


@router.get("/series", response_model=GnssDataSeriesResponse)
@inject
async def gnss_data_series(
    device_id: str,
    gnss_data: FromDishka[GnssDataService],
    start: str | None = Query(None, alias="from"),
    end: str | None = Query(None, alias="to"),
    ids: list[str] | None = Query(None),
    limit_per_dataset: int = Query(10000, ge=1, le=50000),
    current_user: User = Depends(get_current_user),
):
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    if not start_dt or not end_dt:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="from and to are required")
    selected_ids = [item for item in (ids or []) if item]
    if not selected_ids:
        return GnssDataSeriesResponse(items=[], limit_per_dataset=limit_per_dataset)
    series = await gnss_data.series(
        device_id=device_id,
        gnss_data_ids=selected_ids,
        start=start_dt,
        end=end_dt,
        limit_per_dataset=limit_per_dataset,
    )
    return GnssDataSeriesResponse(
        items=[
            GnssDataSeriesOut(
                dataset=GnssDataOut.model_validate(item.dataset, from_attributes=True),
                points=[
                    GnssDataMeasurementPointOut.model_validate(point, from_attributes=True)
                    for point in item.points
                ],
                capped=item.capped,
            )
            for item in series
        ],
        limit_per_dataset=limit_per_dataset,
    )


@router.patch("/{gnss_data_id}", response_model=GnssDataOut)
@inject
async def update_gnss_data(
    device_id: str,
    gnss_data_id: str,
    payload: GnssDataUpdateRequest,
    gnss_data: FromDishka[GnssDataService],
    current_user: User = Depends(get_current_user),
):
    try:
        item = await gnss_data.update(
            device_id=device_id,
            gnss_data_id=gnss_data_id,
            name=payload.name,
            description=payload.description,
            description_set="description" in payload.model_fields_set,
        )
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    except IntegrityError as exc:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="GNSS dataset name already exists") from exc
    if not item:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="GNSS dataset not found")
    return GnssDataOut.model_validate(item, from_attributes=True)


@router.delete("/{gnss_data_id}", status_code=status.HTTP_204_NO_CONTENT)
@inject
async def delete_gnss_data(
    device_id: str,
    gnss_data_id: str,
    gnss_data: FromDishka[GnssDataService],
    current_user: User = Depends(get_current_user),
):
    deleted = await gnss_data.delete(device_id=device_id, gnss_data_id=gnss_data_id)
    if not deleted:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="GNSS dataset not found")


@router.post("/{gnss_data_id}/import", response_model=GnssDataImportResponse)
@inject
async def import_gnss_data(
    device_id: str,
    gnss_data_id: str,
    gnss_data: FromDishka[GnssDataService],
    file: UploadFile = File(...),
    current_user: User = Depends(get_current_user),
):
    content = await file.read()
    try:
        text = content.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="GNSS file must be UTF-8 text") from exc
    try:
        summary = await gnss_data.import_text(device_id=device_id, gnss_data_id=gnss_data_id, raw_text=text)
    except LookupError as exc:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="GNSS dataset not found") from exc
    except GnssDataImportError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail={"errors": exc.errors}) from exc
    return GnssDataImportResponse(
        dataset=GnssDataOut.model_validate(summary.dataset, from_attributes=True),
        parsed_rows=summary.parsed_rows,
        upserted_rows=summary.upserted_rows,
        duplicate_rows=summary.duplicate_rows,
        skipped_rows=summary.skipped_rows,
        first_timestamp=summary.first_timestamp,
        last_timestamp=summary.last_timestamp,
        errors=summary.errors,
    )
