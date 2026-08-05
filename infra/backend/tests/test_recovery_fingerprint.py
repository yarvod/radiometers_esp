from datetime import datetime, timezone

from app.domain.entities import Measurement, MeteoReading
from app.domain.recovery_fingerprint import measurement_recovery_hash, meteo_recovery_hash


def measurement(**overrides) -> Measurement:
    values = {
        "id": "m1",
        "device_id": "dev1",
        "timestamp": datetime(2026, 8, 5, 10, 0, 1, tzinfo=timezone.utc),
        "timestamp_ms": 123,
        "adc1": 1.1234564,
        "adc2": 2.2345674,
        "adc3": 3.3456784,
        "temps": [20.124, 21.235],
        "bus_v": 5.1234,
        "bus_i": 0.2344,
        "bus_p": 1.2344,
        "adc1_cal": None,
        "adc2_cal": None,
        "adc3_cal": None,
        "gps_lat": None,
        "gps_lon": None,
        "gps_alt": None,
        "gps_fix_quality": None,
        "gps_satellites": None,
        "gps_fix_age_ms": None,
        "log_use_motor": False,
        "log_duration": 1.0,
        "log_filename": "data.txt",
    }
    values.update(overrides)
    return Measurement(**values)


def test_measurement_hash_matches_mqtt_and_csv_precision_without_timestamp_ms():
    mqtt = measurement(timestamp_ms=1778067367969)
    csv = measurement(
        id="m2",
        timestamp_ms=None,
        adc1=1.123456,
        adc2=2.234567,
        adc3=3.345678,
        temps=[20.12, 21.23],
        bus_v=5.123,
        bus_i=0.234,
        bus_p=1.234,
    )

    assert measurement_recovery_hash(mqtt) == measurement_recovery_hash(csv)


def test_measurement_hash_keeps_distinct_samples_in_same_iso_second():
    first = measurement(timestamp_ms=100)
    second = measurement(id="m2", timestamp_ms=900, adc2=2.2355674)

    assert first.timestamp == second.timestamp
    assert measurement_recovery_hash(first) != measurement_recovery_hash(second)


def test_meteo_hash_matches_mqtt_and_csv_precision_without_timestamp_ms():
    timestamp = datetime(2026, 8, 5, 10, 0, 1, tzinfo=timezone.utc)
    mqtt = MeteoReading(
        device_id="dev1",
        timestamp=timestamp,
        timestamp_ms=1778067367969,
        light_lux=123.44,
        uvi=1.24,
        temp_c=20.14,
        humidity_pct=55.4,
        wind_speed_ms=2.24,
        gust_speed_ms=3.34,
        wind_dir_deg=180,
        rainfall_mm=0.04,
        pressure_hpa=1000.04,
    )
    csv = MeteoReading(
        device_id="dev1",
        timestamp=timestamp,
        timestamp_ms=None,
        light_lux=123.4,
        uvi=1.2,
        temp_c=20.1,
        humidity_pct=55,
        wind_speed_ms=2.2,
        gust_speed_ms=3.3,
        wind_dir_deg=180,
        rainfall_mm=0.0,
        pressure_hpa=1000.0,
    )

    assert meteo_recovery_hash(mqtt) == meteo_recovery_hash(csv)
