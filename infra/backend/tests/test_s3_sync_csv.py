from datetime import datetime, timedelta, timezone

import pytest

from app.domain.entities import S3ObjectDescriptor, S3SyncObjectState
from app.services.s3_sync import select_pending_objects
from app.services.s3_sync_csv import (
    is_ignored_1970_key,
    parse_meteo_csv,
    parse_radiometer_csv,
)


def test_old_radiometer_header_without_calibration_or_gps_is_supported():
    payload = (
        "timestamp_iso,timestamp_ms,adc1,adc2,adc3,temp1,temp2,bus_v,bus_i,bus_p\n"
        "2026-08-05T10:11:12Z,not-used,1.1,2.2,3.3,20.1,21.2,5.0,0.2,1.0\n"
    ).encode()

    result = parse_radiometer_csv(payload, "dev1", "radiometers/data_20260805_101100_1.txt")

    assert result.invalid_count == 0
    assert result.total_rows == 1
    assert len(result.rows) == 1
    row = result.rows[0]
    assert row.timestamp == datetime(2026, 8, 5, 10, 11, 12, tzinfo=timezone.utc)
    assert row.timestamp_ms is None
    assert row.temps == [20.1, 21.2]
    assert row.adc1_cal is None
    assert row.gps_lat is None
    assert row.log_use_motor is False


def test_current_radiometer_header_uses_labels_and_ignores_new_fields():
    payload = (
        "timestamp_iso,timestamp_ms,adc1,adc2,adc3,outside,load,bus_v,bus_i,bus_p,"
        "adc1_cal,adc2_cal,adc3_cal,gps_lat,gps_lon,gps_alt,gps_fix_quality,gps_satellites,"
        "gps_fix_age_ms,future_field\n"
        "2026-08-05T10:11:12.345Z,999,1,2,3,10,11,5,0.2,1,4,5,6,55.7,37.6,180,4,12,250,x\n"
    ).encode()

    result = parse_radiometer_csv(payload, "dev1", "radiometers/data_20260805_101100_1.txt")

    row = result.rows[0]
    assert row.timestamp.microsecond == 345_000
    assert row.temps == [10.0, 11.0]
    assert (row.adc1_cal, row.adc2_cal, row.adc3_cal) == (4.0, 5.0, 6.0)
    assert row.gps_lat == 55.7
    assert row.gps_satellites == 12
    assert row.log_use_motor is True


def test_radiometer_requires_iso_timestamp_but_keeps_other_valid_rows():
    payload = (
        "timestamp_iso,timestamp_ms,adc1,adc2,adc3,bus_v,bus_i,bus_p\n"
        ",1710000000000,1,2,3,5,0.2,1\n"
        "2026-08-05T10:11:13Z,7,1,2,3,5,0.2,1\n"
    ).encode()

    result = parse_radiometer_csv(payload, "dev1", "radiometers/data_20260805_101100_1.txt")

    assert result.total_rows == 2
    assert result.invalid_count == 1
    assert len(result.rows) == 1
    assert "timestamp_iso is absent" in (result.error_summary or "")


def test_meteo_uses_iso_and_ignores_file_timestamp_ms():
    payload = (
        "timestamp_iso,timestamp_ms,light_lux,uvi,temp_c,humidity_pct,wind_speed_ms,"
        "gust_speed_ms,wind_dir_deg,rainfall_mm,pressure_hpa\n"
        "2026-08-05T10:11:12Z,not-used,120.5,1.2,18.3,56,2.1,3.2,270,0.4,1008.1\n"
    ).encode()

    result = parse_meteo_csv(payload, "dev1")

    row = result.rows[0]
    assert row.timestamp == datetime(2026, 8, 5, 10, 11, 12, tzinfo=timezone.utc)
    assert row.timestamp_ms is None
    assert row.wind_dir_deg == 270
    assert row.pressure_hpa == 1008.1


@pytest.mark.parametrize(
    "key,expected",
    [
        ("radiometers/data_19700101_000000_1.txt", True),
        ("meteo/meteo_19701231_235959_2.txt", True),
        ("radiometers/data_20260805_000000_1.txt", False),
    ],
)
def test_1970_filename_detection(key: str, expected: bool):
    assert is_ignored_1970_key(key) is expected


def test_pending_selection_wraps_and_finds_late_old_object():
    now = datetime.now(timezone.utc)
    objects = [
        S3ObjectDescriptor("radiometers/data_20260801.txt", "new-old", now, 10),
        S3ObjectDescriptor("radiometers/data_20260804.txt", "known", now, 10),
        S3ObjectDescriptor("radiometers/data_20260806.txt", "new", now, 10),
    ]
    states = {
        "radiometers/data_20260804.txt": S3SyncObjectState(
            "radiometers/data_20260804.txt", "known", "done", None
        )
    }

    selected = select_pending_objects(objects, states, "radiometers/data_20260804.txt", now, 10)

    assert [item.key for item in selected] == [
        "radiometers/data_20260806.txt",
        "radiometers/data_20260801.txt",
    ]


def test_pending_selection_defers_failed_object_until_retry_time():
    now = datetime.now(timezone.utc)
    item = S3ObjectDescriptor("radiometers/data_20260805.txt", "etag", now, 10)
    states = {item.key: S3SyncObjectState(item.key, item.etag, "failed", now + timedelta(minutes=5))}

    assert select_pending_objects([item], states, None, now, 10) == []
    assert select_pending_objects([item], states, None, now + timedelta(minutes=6), 10) == [item]
