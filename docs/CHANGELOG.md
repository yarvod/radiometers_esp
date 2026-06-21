# Changelog

## Unreleased

### Firmware
- **WN90LP driver** (`main/wn90lp.h/.cpp`): Modbus RTU over RS485 (UART1, 9600 8N1, GPIO 11/12/13). Bulk-reads all 9 registers in one request every 60 s. Absent/disconnected station is silent — `MeteoData::online = false`, no crash.
- **SharedState**: added `MeteoData meteo` field.
- **CSV log with hourly rotation**: polls write to `{mount}/meteo_YYYYMMDD_HH.txt`. On hour boundary the closed file is moved to `to_upload/` automatically. Works with both SD and internal flash (`ActiveStorageMountPoint()`).
- **S3 upload**: `meteo_*.txt` files are uploaded to bucket prefix `meteo/` (alongside existing `radiometers/` and `gps/` prefixes).
- **Startup cleanup**: `UploadTask` sweeps orphaned `meteo_*.txt` files from root into `to_upload/` on first boot cycle.

## 2026-06-xx (recent)

### Infrastructure (backend)
- Atmosphere coefficient retrieval and management in device settings.
- Temperature outlier filter configuration.
- Backup and restore scripts for Postgres and MinIO.

### Infrastructure (frontend)
- Chart axis controls with auto / min / max for temperature, ADC, brightness, load check, teff, tau, PWV.
- Updated color palettes for atmosphere and PWV profiles in ADC series.
