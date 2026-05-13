from datetime import datetime

from fastapi import APIRouter, Depends, Query
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import MeasurementLatestResponse, MeasurementPointOut, MeasurementsResponse
from app.domain.entities import User
from app.services.brightness import apply_brightness_temperatures
from app.services.calibrations import RadiometerCalibrationService
from app.services.devices import DeviceService
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
    devices: FromDishka[DeviceService],
    calibrations: FromDishka[RadiometerCalibrationService],
    device_id: str = Query(..., min_length=1),
    start: str | None = Query(None, alias="from"),
    end: str | None = Query(None, alias="to"),
    limit: int = Query(2000, ge=1, le=10000),
    bucket_seconds: int | None = Query(None, ge=0, le=86400),
    current_user: User = Depends(get_current_user),
):
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    points_seq, raw_count, bucket_seconds, bucket_label, aggregated = await measurements.list_series(
        device_id=device_id,
        start=start_dt,
        end=end_dt,
        limit=limit,
        bucket_seconds=bucket_seconds if bucket_seconds and bucket_seconds > 0 else None,
    )
    points = list(points_seq)
    calibration_items, _ = await calibrations.list(device_id=device_id, limit=10000, offset=0)
    device = await devices.get_device(device_id)
    config_temp_labels = list(device.temp_labels) if device else []
    config_temp_addresses = list(device.temp_addresses) if device else []
    config_temp_label_map = dict(device.temp_label_map) if device else {}
    config_temp_bindings = dict(device.temp_bindings) if device else {}
    apply_brightness_temperatures(
        points,
        list(calibration_items),
        temp_addresses=config_temp_addresses,
        temp_bindings=config_temp_bindings,
    )
    legacy_labels_by_address = {
        address: label
        for address, label in zip(config_temp_addresses, config_temp_labels)
        if address and label
    }
    default_len = max(len(config_temp_labels), len(config_temp_addresses))
    max_temp = max((len(point.temps) for point in points), default=default_len)
    if len(config_temp_addresses) < max_temp:
        config_temp_addresses.extend([""] * (max_temp - len(config_temp_addresses)))
    temp_labels = []
    for idx in range(max_temp):
        address = config_temp_addresses[idx] if idx < len(config_temp_addresses) else ""
        temp_labels.append(
            (config_temp_label_map.get(address) if address else None)
            or legacy_labels_by_address.get(address)
            or (config_temp_labels[idx] if idx < len(config_temp_labels) and config_temp_labels[idx] else None)
            or f"t{idx + 1}"
        )
    adc_labels = dict(device.adc_labels) if device else {}
    brightness_temp_labels = {
        "brightness_temp1": f"{adc_labels.get('adc1') or 'ADC1'} Tk",
        "brightness_temp2": f"{adc_labels.get('adc2') or 'ADC2'} Tk",
        "brightness_temp3": f"{adc_labels.get('adc3') or 'ADC3'} Tk",
    }
    return MeasurementsResponse(
        points=[MeasurementPointOut.model_validate(item, from_attributes=True) for item in points],
        raw_count=raw_count,
        limit=limit,
        bucket_seconds=bucket_seconds,
        bucket_label=bucket_label,
        aggregated=aggregated,
        temp_labels=temp_labels,
        adc_labels=adc_labels,
        temp_addresses=config_temp_addresses,
        temp_label_map=config_temp_label_map,
        temp_bindings=config_temp_bindings,
        brightness_temp_labels=brightness_temp_labels,
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
