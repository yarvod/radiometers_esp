# Workplan

---

## 1. WN90LP weather station integration

### Done
- [x] `main/wn90lp.h/.cpp` — Modbus RTU over RS485 (UART1, 9600 8N1, GPIO 11/12/13), bulk read 9 registers, CRC16, graceful absent-station handling
- [x] Hourly rotation: `{ActiveStorageMountPoint()}/meteo_YYYYMMDD_HH.txt` → `to_upload/` on hour boundary
- [x] Storage-backend agnostic: uses `ActiveStorageMountPoint()`; safe on SD↔flash switch
- [x] Poll interval from `app_config.meteo_poll_interval_s` (default 60 s, min 10, configurable in `config.txt`)
- [x] `meteo_enabled` flag in `AppConfig` / `config.txt` — set `false` to skip UART init entirely
- [x] `SharedState.meteo` — last reading available to all tasks
- [x] `main/app_main.cpp` — `IsMeteoLogFilename`, `MoveRootMeteoFilesToUploadLocked`, startup sweep, `UploadFileToMinio` routes `meteo_*.txt` → S3 prefix `meteo/`

### TODO — Firmware
- [ ] Wire in `app_main.cpp` (after hardware UART init): `if (app_config.meteo_enabled) { meteo_client.initUart(); meteo_client.startTask(); }`
- [ ] `/data` HTTP response (`http_handlers.cpp`): add meteo fields from SharedState — no new endpoints, just extend existing JSON: `meteoOnline`, `meteoTempC`, `meteoHumidityPct`, `meteoWindSpeedMs`, `meteoWindDirDeg`, `meteoGustSpeedMs`, `meteoPressureHpa`, `meteoLightLux`, `meteoUvi`, `meteoRainfallMm`, `meteoTimestampMs`
- [ ] MQTT `state` publish (`mqtt_bridge.cpp`): include meteo snapshot in `{deviceId}/state` JSON — same fields, NOT a separate topic

### TODO — Backend
- [ ] `worker.py` `handle_state()`: detect `meteoOnline: true` → `devices.upsert_meteo_config(device_id, has_meteo=True)` — same pattern as GPS via `extract_gps_config`
- [ ] DB: `has_meteo: bool` field on `Device` model; migration `000N_meteo_config.py`
- [ ] `repositories/interfaces.py` + `sqlalchemy.py`: `upsert_meteo_config(device_id, has_meteo)`
- [ ] API schema: expose `has_meteo` in `DeviceResponse`
- [ ] **Measurements — DEFERRED**: separate `MeteoReading` table linked to `Device`; don't add to existing `Measurement`/ADC stream (different timescales, different semantics)

### TODO — Frontend
- [ ] `stores/devices.ts`: parse meteo fields from incoming MQTT `state` messages
- [ ] `pages/[deviceId].vue`: "Meteo station" card — visible only when `device.has_meteo == true`, shows last known values reactively from MQTT store

---

## 2. Firmware refactoring — DI and module split

**Goal:** split `app_main.cpp` (4230 lines, 140 KB) into focused modules. Each module is a `.h`/`.cpp` pair with a class that owns its state and receives its dependencies explicitly. Introduce `AppContext` as the DI container passed to all FreeRTOS tasks instead of globals.

### Current problems (from code audit)
- `app_main.cpp` contains 8 unrelated domains: config loading, networking, storage, upload pipeline, data logging, GPS, sensor hub, motion/thermal — all as `static` free functions calling each other
- All tasks receive `nullptr` as `void*` arg and find deps via `extern` globals (`app_config`, `log_file`, `sd_mutex`, etc.)
- `inline constexpr char TAG[] = "APP"` in `app_state.h` leaks into every TU (already caused compile error in `wn90lp.cpp`)
- `SdLockGuard` grabs global `sd_mutex` directly — no injection point
- Task-local state (e.g. `current_gnss_log_path`, `gps_reconfigure_requested`) is file-scope in `app_main.cpp`

### Target: `AppContext` (DI container)

```cpp
// main/app_context.h
struct AppContext {
  AppConfig&          config;
  PidConfig&          pid_config;
  SharedState&        state;
  SemaphoreHandle_t   state_mutex;

  StorageManager      storage;      // owns sd_mutex, mount/path logic
  NetworkManager      network;      // owns wifi/eth/sntp
  UploadPipeline      upload;       // owns minio upload + SigV4
  DataLogger          data_logger;  // owns log_file, current_log_path
  ConfigLoader        config_loader;

  // Optional — nullptr when disabled/absent
  GpsModule*          gps    = nullptr;
  SensorHub*          sensors = nullptr;
  MotionController*   motion  = nullptr;
  Wn90lpClient*       meteo   = nullptr;
};
```

All FreeRTOS tasks get `AppContext*` as `void*` arg:
```cpp
// Before: xTaskCreate(..., nullptr, ...)  → task uses globals
// After:  xTaskCreate(..., &ctx, ...)     → task uses ctx.storage, ctx.config, etc.
```

`SdLockGuard` takes `StorageManager&` instead of grabbing global `sd_mutex`:
```cpp
SdLockGuard guard(ctx.storage);   // was: SdLockGuard guard;
```

### 8 modules to extract (bottom-up migration order)

| Order | Module | Key classes/files | Removes from app_main | Notes |
|---|---|---|---|---|
| 1 | `storage_manager` | `StorageManager` owns `sd_mutex`, `sd_card`, mount/unmount, path helpers, `SdLockGuard` impl | ~250 lines | Foundation for everything else; `SdLockGuard` API changes |
| 2 | `config_loader` | `ConfigLoader` — `ParseConfigText` (371 lines!), `ParseConfigFile`, load/save to SD/flash | ~550 lines | No external deps beyond `AppConfig`/`PidConfig` structs |
| 3 | `network_manager` | `NetworkManager` — wifi/eth init, event handlers, SNTP, `EnsureTimeSynced`, all `wifi_*` file-scope state, 6 fallback tasks | ~700 lines | Largest single domain |
| 4 | `upload_pipeline` | `UploadPipeline` — `UploadFileToMinio` (256 lines), AWS SigV4, `UploadPendingOnce`, file-type routing | ~450 lines | Depends on `StorageManager` + `AppConfig` |
| 5 | `data_logger` | `DataLogger` — `log_file`, `current_log_path`, `OpenLogFileWithPostfix`, `FlushLogFile`, `StopLogging`, CSV row write | ~250 lines | Depends on `StorageManager` |
| 6 | `sensor_hub` | `SensorHub` — LTC2440×3, INA219, 1-Wire, fan tach ISR, `InitSpiBus`, `ReadAllAdc`, adc/ina/temp/fan tasks | ~250 lines | Hardware-only, minimal external deps |
| 7 | `motion_controller` | `MotionController` — stepper, heater/fan PWM, calibration, `StepperTask`, `PidTask`; `control_actions.cpp` becomes thin delegator | ~400 lines | Already partly separate in `control_actions.cpp` |
| 8 | `gps_module` | `GpsModule` wraps `GpsUnicoreClient`; owns gnss log task, rotation, `current_gnss_log_path`; `app_services.h` GPS free functions become methods | ~350 lines | Most complex; depends on storage + upload + time |
| | **Total** | | **~3200 / 4230 (76%)** | `app_main.cpp` shrinks to ~1000 lines |

### Migration rules (apply to each module)
1. Extract class into new `.h`/`.cpp`, add to `CMakeLists.txt`
2. Replace `extern` global accesses with constructor params or `init(deps...)` call
3. File-scope `static` state becomes `private` member
4. Replace `nullptr` task arg with `AppContext*` for that task
5. Each module defines its own `static constexpr char kTag[]` — never `TAG`
6. Build must stay green after each module extraction (incremental, not big-bang)

### Fix `TAG` in `app_state.h` (prerequisite — do first)
Remove `inline constexpr char TAG[] = "APP"` from `app_state.h` (line 28). Add `static constexpr char kTag[] = "APP"` at the top of `app_main.cpp`. Every other `.cpp` already defines its own tag or will when modularised.

### Expected impact on binary size
Modular split with `-ffunction-sections` + LTO means unused methods are stripped per-module. Current pain: `upload_pipeline` and `network_manager` pull in large symbol sets even when some paths are never taken. After split, unreachable code (e.g. fallback network tasks on hardware without WiFi) is more aggressively eliminated. Conservatively: 3–8 KB reduction from dead-code elimination alone.

### Migration order rationale
Start with `storage_manager` (step 1) because `SdLockGuard` is called from nearly every other module — fixing its API unblocks everything. Then `config_loader` (step 2) because it has zero deps and its 550 lines are pure parsing logic. `network_manager` (step 3) last-but-one in the hard deps because wifi/eth event handlers use `esp_event` and are tricky to decouple. `gps_module` (step 8) last because it calls storage + upload + time and its `GpsLogTask` is the most intertwined.

---

## 3. Axis controls (completed in prior releases)
- [x] Auto / min / max axis settings for all chart series
- [x] Temperature outlier filter configuration
