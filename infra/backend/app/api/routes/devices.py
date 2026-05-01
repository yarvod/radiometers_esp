from datetime import datetime

from fastapi import APIRouter, Depends, Query
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import (
    DeviceConfigOut,
    DeviceCreateRequest,
    DeviceGpsConfigOut,
    DeviceGpsConfigUpdateRequest,
    DeviceOut,
    DeviceUpdateRequest,
    ErrorEventOut,
    ErrorEventsResponse,
)
from app.domain.entities import User
from app.services.devices import DeviceService
from app.services.errors import ErrorService

router = APIRouter(prefix="/devices", tags=["devices"])


def sanitize_rtcm_types(values: list[int] | None) -> list[int] | None:
    if values is None:
        return None
    out: list[int] = []
    for value in values:
        try:
            type_id = int(value)
        except (TypeError, ValueError):
            continue
        if 0 < type_id <= 4095 and type_id not in out:
            out.append(type_id)
    return out


@router.get("", response_model=list[DeviceOut])
@inject
async def list_devices(
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    result = await devices.list_devices()
    return [DeviceOut.model_validate(device, from_attributes=True) for device in result]


@router.post("", response_model=DeviceOut)
@inject
async def create_device(
    payload: DeviceCreateRequest,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    device = await devices.create_device(payload.id, payload.display_name)
    return DeviceOut.model_validate(device, from_attributes=True)


@router.patch("/{device_id}", response_model=DeviceConfigOut)
@inject
async def update_device(
    device_id: str,
    payload: DeviceUpdateRequest,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    device = await devices.update_device(
        device_id,
        payload.display_name,
        payload.temp_labels,
        None,
        payload.adc_labels,
    )
    return DeviceConfigOut.model_validate(device, from_attributes=True)


@router.get("/{device_id}", response_model=DeviceConfigOut)
@inject
async def get_device(
    device_id: str,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    device = await devices.get_device(device_id)
    if not device:
        device = await devices.create_device(device_id)
    return DeviceConfigOut.model_validate(device, from_attributes=True)


@router.get("/{device_id}/gps", response_model=DeviceGpsConfigOut)
@inject
async def get_device_gps(
    device_id: str,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    config = await devices.get_gps_config(device_id)
    if not config:
        config = await devices.upsert_gps_config(
            device_id=device_id,
            has_gps=False,
            rtcm_types=[1004, 1006, 1033],
            mode="base_time_60",
            actual_mode=None,
        )
    return DeviceGpsConfigOut.model_validate(config, from_attributes=True)


@router.patch("/{device_id}/gps", response_model=DeviceGpsConfigOut)
@inject
async def update_device_gps(
    device_id: str,
    payload: DeviceGpsConfigUpdateRequest,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    rtcm_types = sanitize_rtcm_types(payload.rtcm_types)
    if payload.rtcm_types is not None and not rtcm_types:
        rtcm_types = [1004, 1006, 1033]
    mode = payload.mode
    if mode is not None and mode not in {"keep", "base_time_60", "base", "rover_uav", "rover"}:
        mode = "base_time_60"
    config = await devices.upsert_gps_config(
        device_id=device_id,
        has_gps=payload.has_gps,
        rtcm_types=rtcm_types,
        mode=mode,
        actual_mode=payload.actual_mode,
    )
    return DeviceGpsConfigOut.model_validate(config, from_attributes=True)


@router.get("/{device_id}/errors", response_model=ErrorEventsResponse)
@inject
async def list_device_errors(
    device_id: str,
    errors: FromDishka[ErrorService],
    start: datetime | None = Query(default=None, alias="from"),
    end: datetime | None = Query(default=None, alias="to"),
    status: str | None = Query(default=None),
    active: bool | None = Query(default=None),
    name: str | None = Query(default=None),
    limit: int = Query(default=200, ge=1, le=5000),
    offset: int = Query(default=0, ge=0),
    current_user: User = Depends(get_current_user),
):
    status_normalized = status.lower().strip() if status else None
    active_filter = active
    if status_normalized:
        if status_normalized in {"active", "open", "on"}:
            active_filter = True
        elif status_normalized in {"cleared", "closed", "off"}:
            active_filter = False
    name_filter = name.strip() if name else None
    total = await errors.count(
        device_id=device_id,
        start=start,
        end=end,
        active=active_filter,
        code=name_filter,
    )
    events = await errors.list(
        device_id=device_id,
        start=start,
        end=end,
        active=active_filter,
        code=name_filter,
        limit=limit,
        offset=offset,
    )
    return ErrorEventsResponse(
        items=[ErrorEventOut.model_validate(event, from_attributes=True) for event in events],
        total=total,
        limit=limit,
        offset=offset,
    )
