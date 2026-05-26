from datetime import datetime, timezone

from app.domain.entities import MeasurementPoint
from app.services.measurements import MeasurementService
from app.services.temp_outliers import TemperatureOutlierFilterConfig, filter_temperature_outliers


def make_point(idx: int, temps: list[float]) -> MeasurementPoint:
    return MeasurementPoint(
        timestamp=datetime(2026, 1, 1, 0, 0, idx, tzinfo=timezone.utc),
        timestamp_ms=idx * 1000,
        adc1=1.0,
        adc2=2.0,
        adc3=3.0,
        temps=temps,
        bus_v=5.0,
        bus_i=0.1,
        bus_p=0.5,
        adc1_cal=None,
        adc2_cal=None,
        adc3_cal=None,
        gps_lat=None,
        gps_lon=None,
        gps_alt=None,
        gps_fix_quality=None,
        gps_satellites=None,
        gps_fix_age_ms=None,
    )


def test_temperature_outlier_filter_removes_spike_from_bound_sensor():
    points = [
        make_point(idx, [20.0, temp])
        for idx, temp in enumerate([10.0, 10.1, 9.9, 10.0, 80.0, 10.1, 10.0, 9.9, 10.0])
    ]

    filtered, stats = filter_temperature_outliers(
        points,
        TemperatureOutlierFilterConfig(enabled=True, window=5, threshold=3.5, min_count=4),
        temp_addresses=["addr-a", "addr-b"],
        temp_bindings={"radiometer_adc2": "addr-b"},
    )

    assert [point.timestamp.second for point in filtered] == [0, 1, 2, 3, 5, 6, 7, 8]
    assert stats.removed_count == 1
    assert stats.inspected_indices == [1]


def test_temperature_outlier_filter_ignores_unbound_sensor_spike():
    points = [
        make_point(idx, [temp, 10.0])
        for idx, temp in enumerate([20.0, 20.1, 20.0, 20.1, 90.0, 20.0, 20.1, 20.0, 20.1])
    ]

    filtered, stats = filter_temperature_outliers(
        points,
        TemperatureOutlierFilterConfig(enabled=True, window=5, threshold=3.5, min_count=4),
        temp_addresses=["addr-a", "addr-b"],
        temp_bindings={"radiometer_adc2": "addr-b"},
    )

    assert filtered == points
    assert stats.removed_count == 0
    assert stats.inspected_indices == [1]


def test_temperature_outlier_filter_disabled_keeps_points():
    points = [make_point(idx, [10.0 if idx != 3 else 100.0]) for idx in range(7)]

    filtered, stats = filter_temperature_outliers(
        points,
        TemperatureOutlierFilterConfig(enabled=False, window=5, threshold=3.5, min_count=4),
    )

    assert filtered == points
    assert stats.removed_count == 0
    assert stats.enabled is False


def test_temperature_outlier_filter_runs_before_bucket_average():
    points = [
        make_point(idx, [temp])
        for idx, temp in enumerate([10.0, 10.1, 9.9, 10.0, 90.0, 10.1, 10.0, 9.9, 10.0])
    ]
    filtered, stats = filter_temperature_outliers(
        points,
        TemperatureOutlierFilterConfig(enabled=True, window=5, threshold=3.5, min_count=4),
    )

    aggregated = MeasurementService._aggregate_points(filtered, bucket_seconds=60, limit=10)

    assert stats.removed_count == 1
    assert len(aggregated) == 1
    assert aggregated[0].temps == [10.0]
