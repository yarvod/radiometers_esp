from __future__ import annotations

import bisect
import math
from dataclasses import dataclass
from datetime import datetime, timedelta

from app.domain.entities import MeasurementPoint, Sounding
from app.repositories.interfaces import (
    DeviceRepository,
    RadiometerCalibrationRepository,
    SoundingRepository,
    StationRepository,
)
from app.services.brightness import apply_brightness_temperatures
from app.services.measurements import MeasurementService


COEFFICIENTS = {
    "3mm": [
        (0.0, {"summer": (0.138, 0.011), "winter": (0.129, 0.009)}),
        (1.0, {"summer": (0.132, 0.010), "winter": (0.126, 0.009)}),
        (2.0, {"summer": (0.125, 0.009), "winter": (0.122, 0.008)}),
        (3.0, {"summer": (0.119, 0.008), "winter": (0.118, 0.007)}),
        (4.0, {"summer": (0.113, 0.007), "winter": (0.113, 0.007)}),
    ],
    "2mm": [
        (0.0, {"summer": (0.082, 0.028), "winter": (0.075, 0.024)}),
        (1.0, {"summer": (0.078, 0.026), "winter": (0.074, 0.023)}),
        (2.0, {"summer": (0.075, 0.024), "winter": (0.072, 0.021)}),
        (3.0, {"summer": (0.071, 0.022), "winter": (0.070, 0.020)}),
        (4.0, {"summer": (0.067, 0.020), "winter": (0.067, 0.018)}),
    ],
}

VALID_BAND_MODES = {"auto", "off", "2mm", "3mm", "manual"}
VALID_SEASONS = {"auto", "summer", "winter"}


@dataclass(frozen=True)
class EffectiveTemperaturePoint:
    station_id: str
    station_name: str | None
    sounding_time: datetime
    t_eff: float | None
    pwv_profile: float | None
    row_count: int


@dataclass(frozen=True)
class AtmosphereMeasurementPoint:
    timestamp: datetime
    timestamp_ms: int | None
    t_eff: float | None
    t_eff_station_id: str | None
    t_eff_age_hours: float | None
    brightness_temp1: float | None
    brightness_temp2: float | None
    brightness_temp3: float | None
    tau1: float | None
    tau2: float | None
    tau3: float | None
    alpha1: float | None
    alpha2: float | None
    alpha3: float | None
    beta1: float | None
    beta2: float | None
    beta3: float | None
    pwv1: float | None
    pwv2: float | None
    pwv3: float | None


@dataclass(frozen=True)
class AtmosphereSeries:
    config: dict[str, object]
    station_labels: dict[str, str]
    adc_labels: dict[str, str]
    t_eff_points: list[EffectiveTemperaturePoint]
    measurement_points: list[AtmosphereMeasurementPoint]
    raw_count: int
    bucket_seconds: int
    bucket_label: str
    aggregated: bool


class AtmosphereService:
    def __init__(
        self,
        devices: DeviceRepository,
        stations: StationRepository,
        soundings: SoundingRepository,
        measurements: MeasurementService,
        calibrations: RadiometerCalibrationRepository,
    ) -> None:
        self._devices = devices
        self._stations = stations
        self._soundings = soundings
        self._measurements = measurements
        self._calibrations = calibrations

    async def build_series(
        self,
        device_id: str,
        start: datetime | None,
        end: datetime | None,
        limit: int,
        bucket_seconds: int | None,
        tau_station_id: str | None,
        average: bool,
        coefficient_config: dict[str, object] | None = None,
    ) -> AtmosphereSeries:
        device = await self._devices.get(device_id)
        config = self._normalize_config(dict(device.atmosphere_config or {}) if device else {})
        if coefficient_config:
            config = self._merge_coefficient_config(config, coefficient_config)
        adc_labels = dict(device.adc_labels or {}) if device else {}
        points, raw_count, bucket_seconds_value, bucket_label, aggregated = await self._measurements.list_series(
            device_id=device_id,
            start=start,
            end=end,
            limit=limit,
            bucket_seconds=bucket_seconds,
        )
        measurement_points = list(points)
        calibrations = list(await self._calibrations.list(device_id=device_id, limit=10000, offset=0))
        apply_brightness_temperatures(
            measurement_points,
            calibrations,
            temp_addresses=list(device.temp_addresses or []) if device else [],
            temp_bindings=dict(device.temp_bindings or {}) if device else {},
        )

        station_ids = [str(item) for item in config.get("station_ids", [])]
        if not station_ids:
            return AtmosphereSeries(
                config=config,
                station_labels={},
                adc_labels=adc_labels,
                t_eff_points=[],
                measurement_points=[],
                raw_count=raw_count,
                bucket_seconds=bucket_seconds_value,
                bucket_label=bucket_label,
                aggregated=aggregated,
            )

        min_ts = min((point.timestamp for point in measurement_points), default=start)
        max_ts = max((point.timestamp for point in measurement_points), default=end)
        sound_start = (min_ts or start) - timedelta(days=3) if (min_ts or start) else None
        sound_end = (max_ts or end) + timedelta(days=3) if (max_ts or end) else None
        altitude_m = float(config.get("altitude_m") or 0.0)
        h0_m = float(config.get("h0_m") or 5300.0)

        station_labels: dict[str, str] = {}
        profiles_by_station: dict[str, list[EffectiveTemperaturePoint]] = {}
        for station_code in station_ids:
            station = await self._stations.get(station_code)
            if not station:
                continue
            station_labels[station.station_id] = station.name or station.station_id
            soundings = list(await self._soundings.list(station.id, sound_start, sound_end, limit=10000, offset=0))
            profiles: list[EffectiveTemperaturePoint] = []
            for sounding in soundings:
                parsed = self._profile_metrics(sounding, altitude_m)
                if parsed is None:
                    continue
                t_eff, pwv_profile = parsed
                profiles.append(
                    EffectiveTemperaturePoint(
                        station_id=station.station_id,
                        station_name=station.name or sounding.station_name,
                        sounding_time=sounding.sounding_time,
                        t_eff=t_eff,
                        pwv_profile=pwv_profile,
                        row_count=sounding.row_count,
                    )
                )
            profiles.sort(key=lambda item: item.sounding_time)
            profiles_by_station[station.station_id] = profiles

        start_key = start.replace(tzinfo=None) if start else None
        end_key = end.replace(tzinfo=None) if end else None
        t_eff_points = [
            point
            for profiles in profiles_by_station.values()
            for point in profiles
            if (start_key is None or point.sounding_time.replace(tzinfo=None) >= start_key)
            and (end_key is None or point.sounding_time.replace(tzinfo=None) <= end_key)
        ]
        t_eff_points.sort(key=lambda item: (item.sounding_time, item.station_id))

        primary_station = (tau_station_id or str(config.get("tau_station_id") or "")).strip()
        if primary_station not in station_ids:
            primary_station = station_ids[0]
        config = {**config, "tau_station_id": primary_station, "tau_average": average}

        series_points = [
            self._measurement_atmosphere_point(
                point=point,
                profiles_by_station=profiles_by_station,
                station_ids=station_ids,
                primary_station=primary_station,
                average=average,
                altitude_m=altitude_m,
                h0_m=h0_m,
                adc_labels=adc_labels,
                config=config,
            )
            for point in measurement_points
        ]
        return AtmosphereSeries(
            config=config,
            station_labels=station_labels,
            adc_labels=adc_labels,
            t_eff_points=t_eff_points,
            measurement_points=series_points,
            raw_count=raw_count,
            bucket_seconds=bucket_seconds_value,
            bucket_label=bucket_label,
            aggregated=aggregated,
        )

    @staticmethod
    def _optional_float(value: object) -> float | None:
        if value is None or value == "":
            return None
        try:
            parsed = float(value)
        except (TypeError, ValueError):
            return None
        return parsed if math.isfinite(parsed) else None

    @classmethod
    def _normalize_band_config(cls, raw: object, default_mode: str = "auto") -> dict[str, object]:
        cfg = raw if isinstance(raw, dict) else {}
        mode = str(cfg.get("mode", default_mode)).strip().lower()
        if mode not in VALID_BAND_MODES:
            mode = default_mode if default_mode in VALID_BAND_MODES else "auto"
        alpha = cls._optional_float(cfg.get("alpha"))
        beta = cls._optional_float(cfg.get("beta"))
        out: dict[str, object] = {"mode": mode}
        if alpha is not None:
            out["alpha"] = alpha
        if beta is not None:
            out["beta"] = beta
        return out

    @classmethod
    def _normalize_config(cls, config: dict[str, object]) -> dict[str, object]:
        station_ids: list[str] = []
        for raw in config.get("station_ids", []) if isinstance(config, dict) else []:
            value = str(raw).strip()
            if value and value not in station_ids:
                station_ids.append(value)
        try:
            altitude_m = float(config.get("altitude_m", 0.0))
        except (TypeError, ValueError, AttributeError):
            altitude_m = 0.0
        try:
            h0_m = float(config.get("h0_m", 5300.0))
        except (TypeError, ValueError, AttributeError):
            h0_m = 5300.0
        tau_station_id = str(config.get("tau_station_id", "")).strip()
        season = str(config.get("season", "auto")).strip().lower()
        if season not in VALID_SEASONS:
            season = "auto"

        bands_config = config.get("bands", {}) if isinstance(config, dict) else {}
        overrides = {
            "adc2": cls._normalize_band_config(
                bands_config.get("adc2") if isinstance(bands_config, dict) else None,
                default_mode="2mm",
            ),
            "adc3": cls._normalize_band_config(
                bands_config.get("adc3") if isinstance(bands_config, dict) else None,
                default_mode="3mm",
            ),
        }
        if isinstance(bands_config, dict) and "adc1" in bands_config:
            overrides["adc1"] = cls._normalize_band_config(bands_config.get("adc1"), default_mode="off")

        return {
            "station_ids": station_ids,
            "altitude_m": max(0.0, altitude_m),
            "h0_m": h0_m if h0_m > 0 else 5300.0,
            "tau_station_id": tau_station_id if tau_station_id in station_ids else (station_ids[0] if station_ids else ""),
            "tau_average": bool(config.get("tau_average", False)),
            "season": season,
            "bands": overrides,
        }

    @classmethod
    def _merge_coefficient_config(cls, base: dict[str, object], raw: dict[str, object]) -> dict[str, object]:
        merged = dict(base)
        season = str(raw.get("season", merged.get("season", "auto"))).strip().lower()
        if season in VALID_SEASONS:
            merged["season"] = season
        raw_bands = raw.get("bands")
        if isinstance(raw_bands, dict):
            bands = dict(merged.get("bands", {})) if isinstance(merged.get("bands"), dict) else {}
            for adc_key in ("adc1", "adc2", "adc3"):
                if adc_key in raw_bands:
                    default_mode = "2mm" if adc_key == "adc2" else "3mm" if adc_key == "adc3" else "off"
                    bands[adc_key] = cls._normalize_band_config(raw_bands.get(adc_key), default_mode=default_mode)
            merged["bands"] = bands
        return merged

    @staticmethod
    def _profile_metrics(sounding: Sounding, min_height_m: float) -> tuple[float, float] | None:
        if sounding.row_count < 50:
            return None
        columns = sounding.columns or []
        col_map = {str(col).split(",", 1)[0].strip(): idx for idx, col in enumerate(columns)}
        h_idx = col_map.get("HGHT")
        temp_idx = col_map.get("TEMP")
        absh_idx = col_map.get("ABSH")
        if h_idx is None or temp_idx is None or absh_idx is None:
            return None
        samples: list[tuple[float, float, float]] = []
        for row in sounding.rows or []:
            try:
                height = float(row[h_idx])
                temp_k = float(row[temp_idx]) + 273.15
                rho = float(row[absh_idx])
            except (TypeError, ValueError, IndexError):
                continue
            if not (math.isfinite(height) and math.isfinite(temp_k) and math.isfinite(rho)):
                continue
            if height < min_height_m or rho < 0:
                continue
            samples.append((height, temp_k, rho))
        if len(samples) < 2:
            return None
        samples.sort(key=lambda item: item[0])
        numerator = 0.0
        denominator = 0.0
        prev_h, prev_t, prev_rho = samples[0]
        for height, temp_k, rho in samples[1:]:
            dh = height - prev_h
            if dh <= 0:
                prev_h, prev_t, prev_rho = height, temp_k, rho
                continue
            denominator += (prev_rho + rho) * 0.5 * dh
            numerator += ((prev_t * prev_rho) + (temp_k * rho)) * 0.5 * dh
            prev_h, prev_t, prev_rho = height, temp_k, rho
        if denominator <= 0:
            return None
        return numerator / denominator, denominator / 1000.0

    @staticmethod
    def _nearest_profile(profiles: list[EffectiveTemperaturePoint], timestamp: datetime) -> EffectiveTemperaturePoint | None:
        if not profiles:
            return None
        ts = timestamp.replace(tzinfo=None)
        times = [item.sounding_time.replace(tzinfo=None) for item in profiles]
        idx = bisect.bisect_left(times, ts)
        candidates: list[EffectiveTemperaturePoint] = []
        if idx > 0:
            candidates.append(profiles[idx - 1])
        if idx < len(profiles):
            candidates.append(profiles[idx])
        if not candidates:
            return None
        return min(candidates, key=lambda item: abs((item.sounding_time.replace(tzinfo=None) - ts).total_seconds()))

    def _measurement_atmosphere_point(
        self,
        point: MeasurementPoint,
        profiles_by_station: dict[str, list[EffectiveTemperaturePoint]],
        station_ids: list[str],
        primary_station: str,
        average: bool,
        altitude_m: float,
        h0_m: float,
        adc_labels: dict[str, str],
        config: dict[str, object],
    ) -> AtmosphereMeasurementPoint:
        nearest: list[EffectiveTemperaturePoint] = []
        if average:
            for station_id in station_ids:
                item = self._nearest_profile(profiles_by_station.get(station_id, []), point.timestamp)
                if item and item.t_eff is not None:
                    nearest.append(item)
        else:
            item = self._nearest_profile(profiles_by_station.get(primary_station, []), point.timestamp)
            if item and item.t_eff is not None:
                nearest.append(item)
        t_eff = sum(float(item.t_eff) for item in nearest) / len(nearest) if nearest else None
        station_label = "avg" if average and nearest else nearest[0].station_id if nearest else None
        age_hours = None
        if nearest:
            ts = point.timestamp.replace(tzinfo=None)
            age_hours = max(abs((item.sounding_time.replace(tzinfo=None) - ts).total_seconds()) for item in nearest) / 3600.0

        brightness = [point.brightness_temp1, point.brightness_temp2, point.brightness_temp3]
        tau_values: list[float | None] = []
        alpha_values: list[float | None] = []
        beta_values: list[float | None] = []
        pwv_values: list[float | None] = []
        for idx, tb in enumerate(brightness, start=1):
            tau = self._calc_tau(tb, t_eff)
            tau_values.append(tau)
            
            adc_key = f"adc{idx}"
            alpha, beta = self._resolve_alpha_beta(adc_key, adc_labels, altitude_m, point.timestamp, config)
            alpha_values.append(alpha)
            beta_values.append(beta)
            
            if tau is None or alpha is None or beta is None:
                pwv_values.append(None)
                continue
                
            dry_tau = alpha * math.exp(-altitude_m / h0_m)
            pwv_values.append((tau - dry_tau) / beta if beta else None)

        return AtmosphereMeasurementPoint(
            timestamp=point.timestamp,
            timestamp_ms=point.timestamp_ms,
            t_eff=t_eff,
            t_eff_station_id=station_label,
            t_eff_age_hours=age_hours,
            brightness_temp1=point.brightness_temp1,
            brightness_temp2=point.brightness_temp2,
            brightness_temp3=point.brightness_temp3,
            tau1=tau_values[0],
            tau2=tau_values[1],
            tau3=tau_values[2],
            alpha1=alpha_values[0],
            alpha2=alpha_values[1],
            alpha3=alpha_values[2],
            beta1=beta_values[0],
            beta2=beta_values[1],
            beta3=beta_values[2],
            pwv1=pwv_values[0],
            pwv2=pwv_values[1],
            pwv3=pwv_values[2],
        )

    @staticmethod
    def _calc_tau(tb: float | None, t_eff: float | None) -> float | None:
        if tb is None or t_eff is None or t_eff <= 0:
            return None
        try:
            ratio = (2.73 / t_eff - 1.0) / (tb / t_eff - 1.0)
            if ratio <= 0 or not math.isfinite(ratio):
                return None
            value = math.log(ratio)
            return value if math.isfinite(value) else None
        except (ValueError, ZeroDivisionError, OverflowError):
            return None

    @staticmethod
    def _season(timestamp: datetime) -> str:
        return "summer" if 5 <= timestamp.month <= 10 else "winter"

    def _resolve_alpha_beta(
        self, 
        adc_key: str, 
        adc_labels: dict[str, str], 
        altitude_m: float, 
        timestamp: datetime,
        config: dict[str, object]
    ) -> tuple[float | None, float | None]:
        bands_config = config.get("bands", {})
        adc_cfg = bands_config.get(adc_key) if isinstance(bands_config, dict) else None

        mode = str(adc_cfg.get("mode", "auto") if isinstance(adc_cfg, dict) else "auto").strip().lower()
        manual_alpha = self._optional_float(adc_cfg.get("alpha")) if isinstance(adc_cfg, dict) else None
        manual_beta = self._optional_float(adc_cfg.get("beta")) if isinstance(adc_cfg, dict) else None

        if mode == "off":
            return None, None
        if mode == "manual":
            return manual_alpha, manual_beta

        band = None
        if mode in ("2mm", "3mm"):
            band = mode
        else:
            label = (adc_labels.get(adc_key) or "").lower().replace(" ", "")
            if "2mm" in label or "2мм" in label:
                band = "2mm"
            elif "3mm" in label or "3мм" in label:
                band = "3mm"
            elif adc_key == "adc2":
                band = "2mm"
            elif adc_key == "adc3":
                band = "3mm"

        if not band:
            return None, None

        season = str(config.get("season", "auto")).strip().lower()
        season_override = season if season in ("summer", "winter") else None
        alpha_beta = self._alpha_beta(band, altitude_m / 1000.0, timestamp, season_override)
        if not alpha_beta:
            return None, None

        alpha, beta = alpha_beta
        if manual_alpha is not None:
            alpha = manual_alpha
        if manual_beta is not None:
            beta = manual_beta

        return alpha, beta

    def _alpha_beta(
        self,
        band: str,
        height_km: float,
        timestamp: datetime,
        season_override: str | None = None,
    ) -> tuple[float, float] | None:
        rows = COEFFICIENTS.get(band)
        if not rows:
            return None
        season = season_override if season_override in ("summer", "winter") else self._season(timestamp)
        if height_km <= rows[0][0]:
            return rows[0][1][season]
        if height_km >= rows[-1][0]:
            return rows[-1][1][season]
        for (h0, vals0), (h1, vals1) in zip(rows, rows[1:]):
            if h0 <= height_km <= h1:
                k = (height_km - h0) / (h1 - h0)
                a0, b0 = vals0[season]
                a1, b1 = vals1[season]
                return a0 + (a1 - a0) * k, b0 + (b1 - b0) * k
        return None
