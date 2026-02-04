from datetime import datetime, timezone

from fastapi import APIRouter, Depends, HTTPException, Query, status
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import (
    StationOut,
    StationRefreshRequest,
    StationUpdateRequest,
    StationsRefreshResponse,
    StationsResponse,
)
from app.domain.entities import User
from app.services.stations import StationService

router = APIRouter(prefix="/stations", tags=["stations"])


@router.get("", response_model=StationsResponse)
@inject
async def list_stations(
    stations: FromDishka[StationService],
    query: str | None = Query(default=None),
    limit: int = Query(default=50, ge=1, le=500),
    offset: int = Query(default=0, ge=0),
    current_user: User = Depends(get_current_user),
):
    total = await stations.count(query=query)
    items = await stations.list(limit=limit, offset=offset, query=query)
    return StationsResponse(
        items=[StationOut.model_validate(item, from_attributes=True) for item in items],
        total=total,
        limit=limit,
        offset=offset,
    )


@router.get("/{station_id}", response_model=StationOut)
@inject
async def get_station(
    station_id: str,
    stations: FromDishka[StationService],
    current_user: User = Depends(get_current_user),
):
    station = await stations.get(station_id)
    if not station:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return StationOut.model_validate(station, from_attributes=True)


@router.patch("/{station_id}", response_model=StationOut)
@inject
async def update_station(
    station_id: str,
    payload: StationUpdateRequest,
    stations: FromDishka[StationService],
    current_user: User = Depends(get_current_user),
):
    station = await stations.update(
        station_id=station_id,
        name=payload.name,
        lat=payload.lat,
        lon=payload.lon,
        src=payload.src,
    )
    return StationOut.model_validate(station, from_attributes=True)


@router.post("/refresh", response_model=StationsRefreshResponse)
@inject
async def refresh_stations(
    stations: FromDishka[StationService],
    payload: StationRefreshRequest | None = None,
    current_user: User = Depends(get_current_user),
):
    dt = (payload.requested_at if payload else None) or datetime.now(timezone.utc)
    updated, fetched = await stations.refresh_for_datetime(dt)
    return StationsRefreshResponse(
        requested_at=dt,
        fetched=fetched,
        updated=updated,
    )


@router.post("/{station_id}/refresh", response_model=StationOut)
@inject
async def refresh_station(
    station_id: str,
    stations: FromDishka[StationService],
    payload: StationRefreshRequest | None = None,
    current_user: User = Depends(get_current_user),
):
    dt = (payload.requested_at if payload else None) or datetime.now(timezone.utc)
    station = await stations.refresh_one(station_id, dt)
    if not station:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return StationOut.model_validate(station, from_attributes=True)
