from datetime import datetime

from fastapi import APIRouter, Depends, HTTPException, Query, status
from fastapi.responses import FileResponse
from starlette.background import BackgroundTask

from pathlib import Path
from dishka.integrations.fastapi import FromDishka, inject

from app.api.deps import get_current_user
from app.api.schemas import (
    SoundingDetailOut,
    SoundingExportJobOut,
    SoundingExportRequest,
    SoundingJobCreateRequest,
    SoundingJobOut,
    SoundingOut,
    SoundingPwvItemOut,
    SoundingPwvRequest,
    SoundingPwvResponse,
    SoundingScheduleConfigOut,
    SoundingScheduleConfigUpdateRequest,
    SoundingScheduleCreateRequest,
    SoundingScheduleItemOut,
    SoundingScheduleResponse,
    SoundingScheduleUpdateRequest,
    SoundingsResponse,
)
from app.domain.entities import User
from app.services.soundings import SoundingService

station_router = APIRouter(prefix="/stations/{station_id}/soundings", tags=["soundings"])
router = APIRouter(prefix="/soundings", tags=["soundings"])


def parse_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    return datetime.fromisoformat(raw)


@station_router.get("", response_model=SoundingsResponse)
@inject
async def list_soundings(
    station_id: str,
    soundings: FromDishka[SoundingService],
    start: str | None = Query(None, alias="from"),
    end: str | None = Query(None, alias="to"),
    limit: int = Query(200, ge=1, le=2000),
    offset: int = Query(0, ge=0),
    current_user: User = Depends(get_current_user),
):
    start_dt = parse_datetime(start)
    end_dt = parse_datetime(end)
    total = await soundings.count_soundings(station_id, start_dt, end_dt)
    items = await soundings.list_soundings(station_id, start_dt, end_dt, limit, offset)
    return SoundingsResponse(
        items=[SoundingOut.model_validate(item, from_attributes=True) for item in items],
        total=total,
        limit=limit,
        offset=offset,
    )


@station_router.get("/{sounding_id}", response_model=SoundingDetailOut)
@inject
async def get_sounding(
    station_id: str,
    sounding_id: str,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    sounding = await soundings.get_sounding(sounding_id)
    if not sounding:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return SoundingDetailOut.model_validate(sounding, from_attributes=True)


@station_router.post("/load", response_model=SoundingJobOut)
@inject
async def create_sounding_job(
    station_id: str,
    payload: SoundingJobCreateRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    try:
        job = await soundings.create_job(
            station_code=station_id,
            start_at=payload.start_at,
            end_at=payload.end_at,
            step_hours=payload.step_hours,
        )
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc))
    return SoundingJobOut.model_validate(job, from_attributes=True)


@station_router.post("/export", response_model=SoundingExportJobOut)
@inject
async def create_export_job(
    station_id: str,
    payload: SoundingExportRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    try:
        job = await soundings.create_export_job(station_code=station_id, sounding_ids=payload.ids)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc))
    return SoundingExportJobOut.model_validate(job, from_attributes=True)


@station_router.post("/pwv", response_model=SoundingPwvResponse)
@inject
async def compute_pwv(
    station_id: str,
    payload: SoundingPwvRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    try:
        items = await soundings.compute_pwv(station_id, payload.ids, payload.min_height)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc))
    return SoundingPwvResponse(
        items=[SoundingPwvItemOut(id=item_id, sounding_time=dt, pwv=pwv) for item_id, dt, pwv in items]
    )


@router.get("/jobs/{job_id}", response_model=SoundingJobOut)
@inject
async def get_sounding_job(
    job_id: str,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    job = await soundings.get_job(job_id)
    if not job:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return SoundingJobOut.model_validate(job, from_attributes=True)


@router.get("/exports/{job_id}", response_model=SoundingExportJobOut)
@inject
async def get_export_job(
    job_id: str,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    job = await soundings.get_export_job(job_id)
    if not job:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    return SoundingExportJobOut.model_validate(job, from_attributes=True)


@router.get("/exports/{job_id}/download")
@inject
async def download_export_job(
    job_id: str,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    job = await soundings.get_export_job(job_id)
    if not job or not job.file_path or job.status != "done":
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")
    file_path = Path(job.file_path)
    if not file_path.exists():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="not_found")

    def cleanup() -> None:
        try:
            file_path.unlink(missing_ok=True)
            parent = file_path.parent
            if parent.exists() and parent.is_dir():
                try:
                    parent.rmdir()
                except OSError:
                    pass
        except OSError:
            pass

    return FileResponse(
        str(file_path),
        filename=job.file_name or "soundings.zip",
        background=BackgroundTask(cleanup),
    )


@router.get("/schedule", response_model=SoundingScheduleResponse)
@inject
async def list_schedule(
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    items = await soundings.list_schedule()
    return SoundingScheduleResponse(
        items=[SoundingScheduleItemOut.model_validate(item, from_attributes=True) for item in items],
    )


@router.post("/schedule", response_model=SoundingScheduleItemOut)
@inject
async def add_schedule(
    payload: SoundingScheduleCreateRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    try:
        item = await soundings.add_schedule(payload.station_id)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc))
    return SoundingScheduleItemOut.model_validate(item, from_attributes=True)


@router.patch("/schedule/{schedule_id}", response_model=SoundingScheduleItemOut)
@inject
async def update_schedule(
    schedule_id: str,
    payload: SoundingScheduleUpdateRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    try:
        item = await soundings.set_schedule_enabled(schedule_id, payload.enabled)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=str(exc))
    return SoundingScheduleItemOut.model_validate(item, from_attributes=True)


@router.delete("/schedule/{schedule_id}")
@inject
async def delete_schedule(
    schedule_id: str,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    await soundings.delete_schedule(schedule_id)
    return {"status": "ok"}


@router.get("/schedule/config", response_model=SoundingScheduleConfigOut)
@inject
async def get_schedule_config(
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    config = await soundings.get_schedule_config()
    return SoundingScheduleConfigOut.model_validate(config, from_attributes=True)


@router.put("/schedule/config", response_model=SoundingScheduleConfigOut)
@inject
async def update_schedule_config(
    payload: SoundingScheduleConfigUpdateRequest,
    soundings: FromDishka[SoundingService],
    current_user: User = Depends(get_current_user),
):
    config = await soundings.update_schedule_config(payload.interval_hours, payload.offset_hours)
    return SoundingScheduleConfigOut.model_validate(config, from_attributes=True)
