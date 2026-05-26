from __future__ import annotations

import math
from dataclasses import dataclass
from statistics import median
from typing import Sequence

from app.domain.entities import MeasurementPoint


@dataclass(frozen=True)
class TemperatureOutlierFilterConfig:
    enabled: bool = False
    window: int = 9
    threshold: float = 3.5
    min_count: int = 5


@dataclass(frozen=True)
class TemperatureOutlierFilterStats:
    enabled: bool
    window: int
    threshold: float
    min_count: int
    inspected_indices: list[int]
    removed_count: int
    input_count: int
    output_count: int

    def as_dict(self) -> dict[str, object]:
        return {
            "enabled": self.enabled,
            "window": self.window,
            "threshold": self.threshold,
            "min_count": self.min_count,
            "inspected_indices": self.inspected_indices,
            "removed_count": self.removed_count,
            "input_count": self.input_count,
            "output_count": self.output_count,
        }


def normalize_filter_config(config: TemperatureOutlierFilterConfig | None) -> TemperatureOutlierFilterConfig:
    if config is None:
        return TemperatureOutlierFilterConfig()
    window = max(3, min(501, int(config.window or 9)))
    if window % 2 == 0:
        window += 1
    threshold = float(config.threshold or 3.5)
    if not math.isfinite(threshold) or threshold <= 0:
        threshold = 3.5
    threshold = min(threshold, 100.0)
    min_count = max(1, min(window - 1, int(config.min_count or 5)))
    return TemperatureOutlierFilterConfig(
        enabled=bool(config.enabled),
        window=window,
        threshold=threshold,
        min_count=min_count,
    )


def bound_temperature_indices(
    points: Sequence[MeasurementPoint],
    temp_addresses: Sequence[str] | None = None,
    temp_bindings: dict[str, str] | None = None,
) -> list[int]:
    max_temp = max((len(point.temps) for point in points), default=0)
    if max_temp <= 0:
        return []

    addresses = list(temp_addresses or [])
    bindings = dict(temp_bindings or {})
    indices: list[int] = []
    for role in ("radiometer_adc1", "radiometer_adc2", "radiometer_adc3", "calibration_load"):
        address = str(bindings.get(role) or "").strip()
        if not address:
            continue
        try:
            idx = addresses.index(address)
        except ValueError:
            continue
        if 0 <= idx < max_temp and idx not in indices:
            indices.append(idx)

    if indices:
        return indices
    return list(range(max_temp))


def filter_temperature_outliers(
    points: Sequence[MeasurementPoint],
    config: TemperatureOutlierFilterConfig | None,
    temp_addresses: Sequence[str] | None = None,
    temp_bindings: dict[str, str] | None = None,
) -> tuple[list[MeasurementPoint], TemperatureOutlierFilterStats]:
    normalized = normalize_filter_config(config)
    input_count = len(points)
    inspected_indices = bound_temperature_indices(points, temp_addresses, temp_bindings) if normalized.enabled else []
    if not normalized.enabled or not points or not inspected_indices:
        return list(points), TemperatureOutlierFilterStats(
            enabled=normalized.enabled,
            window=normalized.window,
            threshold=normalized.threshold,
            min_count=normalized.min_count,
            inspected_indices=inspected_indices,
            removed_count=0,
            input_count=input_count,
            output_count=input_count,
        )

    radius = normalized.window // 2
    outlier_rows: set[int] = set()
    for temp_idx in inspected_indices:
        values = [
            float(point.temps[temp_idx])
            if temp_idx < len(point.temps) and _finite(point.temps[temp_idx])
            else None
            for point in points
        ]
        for row_idx, value in enumerate(values):
            if value is None:
                continue
            start = max(0, row_idx - radius)
            end = min(len(values), row_idx + radius + 1)
            neighbors = [
                item
                for idx, item in enumerate(values[start:end], start=start)
                if idx != row_idx and item is not None
            ]
            if len(neighbors) < normalized.min_count:
                continue
            center = float(median(neighbors))
            deviations = [abs(item - center) for item in neighbors]
            mad = float(median(deviations))
            delta = abs(value - center)
            if mad > 0:
                robust_sigma = 0.6745 * delta / mad
                if robust_sigma > normalized.threshold:
                    outlier_rows.add(row_idx)
            elif delta > normalized.threshold:
                outlier_rows.add(row_idx)

    filtered = [point for idx, point in enumerate(points) if idx not in outlier_rows]
    return filtered, TemperatureOutlierFilterStats(
        enabled=normalized.enabled,
        window=normalized.window,
        threshold=normalized.threshold,
        min_count=normalized.min_count,
        inspected_indices=inspected_indices,
        removed_count=len(outlier_rows),
        input_count=input_count,
        output_count=len(filtered),
    )


def _finite(value: object) -> bool:
    try:
        parsed = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return False
    return math.isfinite(parsed)
