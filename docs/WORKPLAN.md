# Workplan

## WN90LP weather station integration

### Done
- [x] `main/wn90lp.h` — `MeteoData` struct, `Wn90lpClient` class, `AppendMeteoLog` declaration
- [x] `main/wn90lp.cpp` — Modbus RTU over RS485, bulk read of all 9 registers (0x0165–0x016D), CRC16, graceful absent-station handling, 60-second polling task
- [x] `main/wn90lp.cpp` — CSV log with **hourly rotation** to `{ActiveStorageMountPoint()}/meteo_YYYYMMDD_HH.txt`; on hour change, completed file is renamed into `ActiveToUploadDir()` for S3 upload
- [x] `main/wn90lp.cpp` — uses `ActiveStorageMountPoint()` so it works with both SD and internal flash
- [x] `main/app_state.h` — added `MeteoData meteo` field to `SharedState`
- [x] `main/app_main.cpp` — `IsMeteoLogFilename()` helper, `MoveRootMeteoFilesToUploadLocked()` (startup cleanup of orphaned files), `UploadTask` calls it once at boot
- [x] `main/app_main.cpp` — `UploadFileToMinio`: `meteo_*.txt` → S3 prefix `meteo/` in bucket

### TODO
- [ ] Wire `Wn90lpClient` into `app_main.cpp`:
  - call `meteo_client.initUart()` during hardware init
  - call `meteo_client.startTask()` after SD is mounted
  - expose `extern Wn90lpClient meteo_client;` or pass via global
- [ ] Expose meteo fields in `/data` HTTP endpoint (`http_handlers.cpp`):
  - `meteo_online`, `meteo_temp_c`, `meteo_humidity_pct`, `meteo_wind_speed_ms`,
    `meteo_wind_dir_deg`, `meteo_gust_speed_ms`, `meteo_pressure_hpa`,
    `meteo_light_lux`, `meteo_uvi`, `meteo_rainfall_mm`, `meteo_timestamp_ms`
- [ ] Publish meteo data in MQTT bridge (`mqtt_bridge.cpp`):
  - add meteo fields to the periodic `{deviceId}/state` publish
- [ ] Frontend: add a "Meteo" panel on the device page (`pages/[deviceId].vue`)
- [ ] Backend: add meteo columns to `Measurement` entity/model and migration,
  or store as a separate `MeteoMeasurement` table
- [ ] Backend: include meteo fields in the MQTT `measure`/`state` handler (`worker.py`)
- [ ] Config option in `config.txt` to disable the meteo task entirely
  (for setups without the module attached at all, not just powered off)

## Axis controls (completed in prior releases)
- [x] Auto / min / max axis settings for all chart series
- [x] Temperature outlier filter configuration
