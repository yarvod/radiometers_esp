from datetime import datetime

from fastapi import APIRouter, Depends, HTTPException, Query, status
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
    RadiometerCalibrationCreateRequest,
    RadiometerCalibrationOut,
    RadiometerCalibrationUpdateRequest,
    RadiometerCalibrationsResponse,
)
from app.domain.entities import User
from app.services.calibrations import CalibrationError, RadiometerCalibrationService
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
        payload.temp_addresses,
        payload.temp_label_map,
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


@router.get("/{device_id}/calibrations", response_model=RadiometerCalibrationsResponse)
@inject
async def list_radiometer_calibrations(
    device_id: str,
    calibrations: FromDishka[RadiometerCalibrationService],
    limit: int = Query(default=20, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
    current_user: User = Depends(get_current_user),
):
    items, total = await calibrations.list(device_id=device_id, limit=limit, offset=offset)
    return RadiometerCalibrationsResponse(
        items=[RadiometerCalibrationOut.model_validate(item, from_attributes=True) for item in items],
        total=total,
        limit=limit,
        offset=offset,
    )


@router.post("/{device_id}/calibrations", response_model=RadiometerCalibrationOut, status_code=status.HTTP_201_CREATED)
@inject
async def create_radiometer_calibration(
    device_id: str,
    payload: RadiometerCalibrationCreateRequest,
    calibrations: FromDishka[RadiometerCalibrationService],
    current_user: User = Depends(get_current_user),
):
    try:
        calibration = await calibrations.create(
            device_id=device_id,
            t_black_body_1=payload.t_black_body_1,
            t_black_body_2=payload.t_black_body_2,
            adc1_1=payload.adc1_1,
            adc2_1=payload.adc2_1,
            adc3_1=payload.adc3_1,
            adc1_2=payload.adc1_2,
            adc2_2=payload.adc2_2,
            adc3_2=payload.adc3_2,
            t_adc1=payload.t_adc1,
            t_adc2=payload.t_adc2,
            t_adc3=payload.t_adc3,
            comment=payload.comment,
        )
    except CalibrationError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    return RadiometerCalibrationOut.model_validate(calibration, from_attributes=True)


@router.patch("/{device_id}/calibrations/{calibration_id}", response_model=RadiometerCalibrationOut)
@inject
async def update_radiometer_calibration(
    device_id: str,
    calibration_id: str,
    payload: RadiometerCalibrationUpdateRequest,
    calibrations: FromDishka[RadiometerCalibrationService],
    current_user: User = Depends(get_current_user),
):
    try:
        calibration = await calibrations.update(
            device_id=device_id,
            calibration_id=calibration_id,
            t_black_body_1=payload.t_black_body_1,
            t_black_body_2=payload.t_black_body_2,
            adc1_1=payload.adc1_1,
            adc2_1=payload.adc2_1,
            adc3_1=payload.adc3_1,
            adc1_2=payload.adc1_2,
            adc2_2=payload.adc2_2,
            adc3_2=payload.adc3_2,
            t_adc1=payload.t_adc1,
            t_adc2=payload.t_adc2,
            t_adc3=payload.t_adc3,
            comment=payload.comment,
        )
    except CalibrationError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    if calibration is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="calibration not found")
    return RadiometerCalibrationOut.model_validate(calibration, from_attributes=True)


@router.delete("/{device_id}/calibrations/{calibration_id}", status_code=status.HTTP_204_NO_CONTENT)
@inject
async def delete_radiometer_calibration(
    device_id: str,
    calibration_id: str,
    calibrations: FromDishka[RadiometerCalibrationService],
    current_user: User = Depends(get_current_user),
):
    deleted = await calibrations.delete(device_id=device_id, calibration_id=calibration_id)
    if not deleted:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="calibration not found")
    return None


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
