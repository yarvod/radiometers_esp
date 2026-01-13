from fastapi import APIRouter, Depends
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import DeviceCreateRequest, DeviceOut, DeviceUpdateRequest
from app.domain.entities import User
from app.services.devices import DeviceService

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


@router.patch("/{device_id}", response_model=DeviceOut)
@inject
async def update_device(
    device_id: str,
    payload: DeviceUpdateRequest,
    devices: FromDishka[DeviceService],
    current_user: User = Depends(get_current_user),
):
    device = await devices.update_device(device_id, payload.display_name)
    return DeviceOut.model_validate(device, from_attributes=True)
