from __future__ import annotations

import math
import uuid
from datetime import datetime, timezone
from typing import Sequence

from app.domain.entities import RadiometerCalibration
from app.repositories.interfaces import RadiometerCalibrationRepository


class CalibrationError(ValueError):
    pass


class RadiometerCalibrationService:
    def __init__(self, calibrations: RadiometerCalibrationRepository) -> None:
        self._calibrations = calibrations

    async def list(self, device_id: str, limit: int, offset: int) -> tuple[Sequence[RadiometerCalibration], int]:
        total = await self._calibrations.count(device_id=device_id)
        items = await self._calibrations.list(device_id=device_id, limit=limit, offset=offset)
        return items, total

    async def create(
        self,
        *,
        device_id: str,
        t_black_body_1: float,
        t_black_body_2: float,
        adc1_1: float,
        adc2_1: float,
        adc3_1: float,
        adc1_2: float,
        adc2_2: float,
        adc3_2: float,
        t_adc1: float,
        t_adc2: float,
        t_adc3: float,
        comment: str | None,
    ) -> RadiometerCalibration:
        values = [
            t_black_body_1,
            t_black_body_2,
            adc1_1,
            adc2_1,
            adc3_1,
            adc1_2,
            adc2_2,
            adc3_2,
            t_adc1,
            t_adc2,
            t_adc3,
        ]
        if not all(math.isfinite(value) for value in values):
            raise CalibrationError("calibration values must be finite")

        adc1_slope, adc1_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc1_1, adc1_2, "adc1")
        adc2_slope, adc2_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc2_1, adc2_2, "adc2")
        adc3_slope, adc3_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc3_1, adc3_2, "adc3")

        calibration = RadiometerCalibration(
            id=str(uuid.uuid4()),
            device_id=device_id,
            created_at=datetime.now(timezone.utc),
            t_black_body_1=t_black_body_1,
            t_black_body_2=t_black_body_2,
            adc1_1=adc1_1,
            adc2_1=adc2_1,
            adc3_1=adc3_1,
            adc1_2=adc1_2,
            adc2_2=adc2_2,
            adc3_2=adc3_2,
            t_adc1=t_adc1,
            t_adc2=t_adc2,
            t_adc3=t_adc3,
            adc1_slope=adc1_slope,
            adc2_slope=adc2_slope,
            adc3_slope=adc3_slope,
            adc1_intercept=adc1_intercept,
            adc2_intercept=adc2_intercept,
            adc3_intercept=adc3_intercept,
            comment=comment.strip() if comment and comment.strip() else None,
        )
        return await self._calibrations.create(calibration)

    async def update(
        self,
        *,
        device_id: str,
        calibration_id: str,
        t_black_body_1: float,
        t_black_body_2: float,
        adc1_1: float,
        adc2_1: float,
        adc3_1: float,
        adc1_2: float,
        adc2_2: float,
        adc3_2: float,
        t_adc1: float,
        t_adc2: float,
        t_adc3: float,
        comment: str | None,
    ) -> RadiometerCalibration | None:
        existing = await self._calibrations.get(device_id=device_id, calibration_id=calibration_id)
        if existing is None:
            return None
        values = [
            t_black_body_1,
            t_black_body_2,
            adc1_1,
            adc2_1,
            adc3_1,
            adc1_2,
            adc2_2,
            adc3_2,
            t_adc1,
            t_adc2,
            t_adc3,
        ]
        if not all(math.isfinite(value) for value in values):
            raise CalibrationError("calibration values must be finite")

        adc1_slope, adc1_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc1_1, adc1_2, "adc1")
        adc2_slope, adc2_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc2_1, adc2_2, "adc2")
        adc3_slope, adc3_intercept = self._linear_coefficients(t_black_body_1, t_black_body_2, adc3_1, adc3_2, "adc3")

        calibration = RadiometerCalibration(
            id=existing.id,
            device_id=existing.device_id,
            created_at=existing.created_at,
            t_black_body_1=t_black_body_1,
            t_black_body_2=t_black_body_2,
            adc1_1=adc1_1,
            adc2_1=adc2_1,
            adc3_1=adc3_1,
            adc1_2=adc1_2,
            adc2_2=adc2_2,
            adc3_2=adc3_2,
            t_adc1=t_adc1,
            t_adc2=t_adc2,
            t_adc3=t_adc3,
            adc1_slope=adc1_slope,
            adc2_slope=adc2_slope,
            adc3_slope=adc3_slope,
            adc1_intercept=adc1_intercept,
            adc2_intercept=adc2_intercept,
            adc3_intercept=adc3_intercept,
            comment=comment.strip() if comment and comment.strip() else None,
        )
        return await self._calibrations.update(calibration)

    async def delete(self, device_id: str, calibration_id: str) -> bool:
        return await self._calibrations.delete(device_id=device_id, calibration_id=calibration_id)

    @staticmethod
    def _linear_coefficients(t1: float, t2: float, adc1: float, adc2: float, name: str) -> tuple[float, float]:
        denominator = adc2 - adc1
        if abs(denominator) < 1e-12:
            raise CalibrationError(f"{name} calibration points have equal ADC values")
        slope = (t2 - t1) / denominator
        intercept = t1 - slope * adc1
        return slope, intercept
