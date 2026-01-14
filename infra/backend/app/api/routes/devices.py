from datetime import datetime

from fastapi import APIRouter, Depends, Query
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import (
    DeviceConfigOut,
    DeviceCreateRequest,
    DeviceOut,
    DeviceUpdateRequest,
    ErrorEventOut,
    ErrorEventsResponse,
)
from app.domain.entities import User
from app.services.devices import DeviceService
from app.services.errors import ErrorService

router = APIRouter(prefix="/devices", tags=["devices"])


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
