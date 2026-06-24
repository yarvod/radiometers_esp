# Workplan

---

## Firmware refactoring — DI / module extraction

**Goal:** `app_main.cpp` (3740 lines at start) → entry point ~300 lines. Each domain becomes an isolated `.h`/`.cpp` module.

### Acceptance criteria (every phase)
1. `rtk idf.py build` exits 0 — zero errors, zero new warnings
2. Binary size ≤ previous phase + 2 KB (or justified in commit message)
3. Commit message records: lines removed from `app_main.cpp`, binary size

---

### Phase 0 — Tag cleanup ✅ DONE
- [x] Remove `inline constexpr char TAG[]` from `app_state.h` (leaked into every TU)
- [x] `app_main.cpp`: add `static constexpr char kTag[] = "APP"`, replace 153 `TAG` → `kTag`
- [x] `http_handlers.cpp`: add `static constexpr char kTag[] = "HTTP"`, replace 48 `TAG` → `kTag`
- [x] `wn90lp.cpp`: already uses `kTag` — no collision
- [x] Build green, committed `refactor(fw): replace global TAG with file-local kTag`

---

### Phase 1 — AppContext skeleton + ConfigLoader ✅ DONE
- [x] Create `main/app_context.h` — `AppContext` struct (DI container skeleton)
- [x] Create `main/config_loader.h` — declarations for `LoadConfig*`, `SaveConfig*`
- [x] Create `main/config_loader.cpp` — move all config load/save/parse logic from `app_main.cpp`
- [x] Remove extracted functions from `app_main.cpp` (~250 lines)
- [x] Update `main/CMakeLists.txt` — add `config_loader.cpp`
- [x] Build green, committed `refactor(fw): extract ConfigLoader`

---

### Phase 2 — StorageManager ✅ DONE
- [x] Create `main/storage_manager.h` — `SdLockGuard`, `MountLogSd`, `MountInternalFlashFs`, `ActiveStorageMountPoint`, path helpers
- [x] Create `main/storage_manager.cpp` — owns `s_sd_mutex`, `s_log_sd_card`, `s_log_sd_mounted`, `s_flash_mounted`, `s_flash_wl`
- [x] Remove `SdLockGuard` impl and `sd_mutex` / `log_sd_card` / `log_sd_mounted` from `app_state.h/cpp`
- [x] Remove storage functions from `http_handlers.cpp` (`MountLogSd`, `MountInternalFlashFs`, path helpers) — `-147 lines`
- [x] Replace `log_sd_mounted` → `IsLogSdMounted()`, `internal_flash_fs_mounted` → `IsInternalFlashMounted()` across all callers
- [x] Replace `sd_mutex = xSemaphoreCreateMutex()` → `StorageManagerInit()` in `app_main.cpp`
- [x] Update `main/CMakeLists.txt` — add `storage_manager.cpp`
- [x] Build green, committed `refactor(fw): extract StorageManager`

---

### Phase 3 — NetworkManager ✅ DONE
- [x] Create `main/network_manager.h` — `InitWifi`, `ApplyNetworkConfig`, `StartSntp`, `EnsureTimeSynced`, `IsSntpUsable`, `StartNetworkTasks`
- [x] Create `main/network_manager.cpp` — owns all Wi-Fi/Eth/SNTP globals, event handlers, monitor tasks (~750 lines)
- [x] Remove 941 lines from `app_main.cpp`: Wi-Fi/Eth globals, `FormatIp4`…`ApplyNetworkConfig`, `WifiRecoverTask`…`EnableFallbackAp`, `WifiEventHandler`…`EnsureTimeSynced` (3722 → 2781 lines)
- [x] Replace `WaitForTimeSyncMs(8000)` → `EnsureTimeSynced(8000)` in `app_main.cpp`
- [x] Replace `xTaskCreatePinnedToCore(&NetworkMonitorTask,...)` + `xTaskCreatePinnedToCore(&WifiMonitorTask,...)` → `StartNetworkTasks()`
- [x] Add `#include "network_manager.h"` to `app_main.cpp` and `app_services.h`
- [x] Remove `InitWifi`, `ApplyNetworkConfig`, `EnsureTimeSynced` forward decls from `app_services.h`
- [x] Add `efuse` to `PRIV_REQUIRES` in `CMakeLists.txt` (needed by `esp_efuse_mac_get_default`)
- [x] Update `main/CMakeLists.txt` — add `network_manager.cpp`
- [x] Build green: binary `0x16cab0` (prev `0x16cf90`, -224 bytes), committed `refactor(fw): extract NetworkManager (Phase 3)`

---

### Phase 4 — UploadPipeline ✅ DONE
- [x] Create `main/upload_pipeline.h` — `UpdateSdStatsLocked`, `UpdateSdStats`, `SdStatsTask`, `QueueCurrentLogForUpload`, `StartUploadedClearTask`, `UploadTask`, `LogMemoryStatus`, `MbedtlsAllocModeName`
- [x] Create `main/upload_pipeline.cpp` — SigV4/HMAC helpers, `UploadFileToMinio`, `UploadPendingOnce`, `UploadTask`, S3-stats, `UploadedClearTask` (~700 lines)
- [x] Remove 855 lines from `app_main.cpp` (2781 → 1926)
- [x] Add `#include "upload_pipeline.h"` to `app_services.h`; remove old declarations
- [x] Add `upload_pipeline.cpp` to `CMakeLists.txt`
- [x] Build green, committed `refactor(fw): extract UploadPipeline (Phase 4)`

---

### Phase 5 — DataLogger ✅ DONE
- [x] Create `main/data_logger.h` — `BuildLogFilename`, `FlushLogFile`, `OpenLogFileWithPostfix`
- [x] Create `main/data_logger.cpp` — ~110 lines; owns log filename/header construction
- [x] Remove 100 lines from `app_main.cpp` (1926 → 1827); replace `WaitForTimeSyncMs` → `EnsureTimeSynced`
- [x] Add `#include "data_logger.h"` to `app_services.h`
- [x] Add `data_logger.cpp` to `CMakeLists.txt`
- [x] Build green, committed `refactor(fw): extract DataLogger (Phase 5)`

---

### Phase 6 — SensorHub ✅ DONE
- [x] Create `main/sensor_hub.h` — `InitSpiBus`, `EnsureGpioIsrServiceInstalled`, `SensorHubInitGpios`, `SensorHubInitAdcs`, `SensorHubInitIna`, `WaitForTempSensors`, `ReadAllAdc`, `SensorHubStartTasks`
- [x] Create `main/sensor_hub.cpp` — LTC2440×3, INA219 I2C, 1-Wire TempTask, fan tach ISR; private: `ReadAllAdc`, `ReadIna219`, `BuildTempMeta`, task functions (~370 lines)
- [x] Remove 356 lines from `app_main.cpp` (1827 → 1460); replace 4 `xTaskCreate` calls with `SensorHubStartTasks()`
- [x] Replace forward-decl of `InitSpiBus` in `network_manager.cpp` with `#include "sensor_hub.h"`
- [x] Add `#include "sensor_hub.h"` to `app_services.h`, `CMakeLists.txt`
- [x] Build green: binary `0x16cb20` (unchanged vs Phase 5), zero warnings

---

### Phase 7 — MotionController ✅ DONE
- [x] Create `main/motion_controller.h` — `MotionControllerInit`, `MotionControllerStartTasks`, stepper primitives, heater/fan/GPIO, `CalibrateZero`, `CalibrationTask`, `FindZeroTask`, `StartFindZeroTask`, `StopLogging`, `StartLoggingToFile`
- [x] Create `main/motion_controller.cpp` — heater/fan LEDC PWM, `StepperTask`, `PidTask`, all homing helpers (moved from http_handlers anonymous namespace), `LoggingTask` (~680 lines)
- [x] Remove 382 lines from `app_main.cpp` (1460 → 1075); replace `InitHeaterPwm+InitFanPwm` with `MotionControllerInit()`, `StepperTask+PidTask` with `MotionControllerStartTasks()`
- [x] Remove 524 lines of motion/logging code from `http_handlers.cpp` (3072 → 2478)
- [x] Remove orphaned helper functions from `http_handlers.cpp` (70 more lines)
- [x] Add `#include "motion_controller.h"` to `app_services.h`, `CMakeLists.txt`
- [x] Build green: binary `0x16c960` (shrank 448 bytes), zero warnings

---

### Phase 8 — GpsModule ✅ DONE
- [x] Move `UtcTimeSnapshot`, `UtcTimeSource`, `GpsPositionSnapshot`, `GpsReceiverStatus`, `ClearUploadedFilesResult` from `app_services.h` to `app_state.h` (breaks potential circular include)
- [x] Create `main/gps_module.h` — `StartGpsModule`, `StartGpsLogTask`, GPS query functions, UTC time utilities, `IsGpsLogFilename`, `IsMeteoLogFilename`
- [x] Create `main/gps_module.cpp` — all UTC time math (DaysFromCivil, GpsDateTimeToUnix, MaybeDisciplineSystemTimeFromGps), `GetBestUtcTimeForData/Gps`, `UtcTimeSourceName`, `UtcTimeToUnixMs`, GPS receiver control, GNSS log helpers, `GpsLogTask` (~430 lines)
- [x] Remove 591 lines from `app_main.cpp` (1075 → 484); replace `StartGpsClient+GpsLogTask create` with `StartGpsModule()` + `StartGpsLogTask(mode)`
- [x] Refactor `app_services.h` to only include domain headers; clean up struct definitions
- [x] Add `gps_module.cpp` to `CMakeLists.txt`
- [x] Build green: binary `0x16c8d0` (-144 bytes vs Phase 7), zero warnings; **app_main.cpp at 483 lines ✅**

---

### Phase 9 — Final cleanup ✅ DONE
- [x] Wire `Wn90lpClient`: `static Wn90lpClient s_meteo_client`; `initUart()` + `startTask()` gated on `meteo_enabled && METEO_RS485_TX != GPIO_NUM_NC`
- [x] Prune ~25 unused `#include` directives from `app_main.cpp`; remove dead `#if 0` block
- [x] Build green: binary `0x16dc60` (+5 KB for Wn90lp UART linkage), zero warnings
- [x] **app_main.cpp: 437 lines ✅ (target: ≤500)**

---

## Backend / Frontend features

### Meteo station (WN90LP) integration
- [ ] Detect `meteoOnline: true` in MQTT state → `has_meteo: bool` on Device model
- [ ] `WN90LP` task publishes `MeteoData` into `SharedState`
- [ ] `/data` HTTP response: add meteo fields from `SharedState`
- [ ] MQTT `state` publish: include meteo snapshot in `{deviceId}/state` JSON
- [ ] Backend: `upsert_meteo_config`, `has_meteo` on Device, migration
- [ ] Frontend: "Meteo station" card on device detail (visible when `has_meteo == true`)
- [ ] **Measurements — DEFERRED**: standalone `MeteoReading` table; don't mix ADC stream

---

### Phase 10 — ESP-IDF Component System migration ✅ DONE

Each flat module moved from `main/` to a proper `components/NAME/` directory.

Final layout:
```
components/
  app_core/          ← app_state, app_utils, error_manager, hw_pins, app_context
  storage_manager/   PRIV_REQUIRES app_core
  config_loader/     REQUIRES app_core; PRIV_REQUIRES storage_manager
  network_manager/   PRIV_REQUIRES app_core, storage_manager, sensor_hub
  sensor_hub/        PRIV_REQUIRES app_core, storage_manager
  upload_pipeline/   PRIV_REQUIRES app_core, storage_manager, gps_module, data_logger
  data_logger/       PRIV_REQUIRES app_core, storage_manager, gps_module, network_manager, sensor_hub
  gps_module/        REQUIRES app_core; PRIV_REQUIRES storage_manager, network_manager, upload_pipeline
  motion_controller/ REQUIRES app_core; PRIV_REQUIRES storage_manager, data_logger, sensor_hub, upload_pipeline, gps_module
main/                ← app_main, http_handlers, web_ui, control_actions, mqtt_bridge, wn90lp
                       REQUIRES all components above
```

Pre-migration decoupling fixes:
- [x] `MeteoData` moved from `wn90lp.h` → `app_state.h`; `wn90lp.h` includes `app_state.h`
- [x] `app_state.h`: replace `#include "storage_manager.h"` with `#include "sdmmc_cmd.h"`
- [x] UTC utilities (`FormatUtcIso`, `UtcTimeToUnixMs`, `UtcTimeSourceName`) moved to `app_utils`
- [x] `IsoUtcNow` (dead code, never called) removed entirely
- [x] `Basename`, `MoveFileToDir` moved from `app_main.cpp` → `app_utils.cpp`
- [x] `error_manager`: `UtcTimeGetterFn` + `ErrorManagerSetTimeGetter` to break GPS dep
- [x] `motion_controller`: `MeasurementPublishFn` + `MotionControllerSetPublisher` to break mqtt_bridge dep
- [x] `data_logger`: removed `UpdateSdStatsLocked()` call to break circular dep on `upload_pipeline`

Component tasks:
- [x] Create `components/app_core/` (app_state, app_utils, error_manager, hw_pins, app_context)
- [x] Create `components/config_loader/`
- [x] Create `components/storage_manager/`
- [x] Create `components/network_manager/`
- [x] Create `components/sensor_hub/` (includes ltc2440, onewire_m1820)
- [x] Create `components/upload_pipeline/` (includes sd_maintenance, TLS certs)
- [x] Create `components/data_logger/`
- [x] Create `components/motion_controller/`
- [x] Create `components/gps_module/` (includes gps_unicore)
- [x] Clean up `main/CMakeLists.txt` — 6 SRCS + REQUIRES all components
- [x] Build green: binary `0x16dc90` (+48 bytes vs Phase 9), zero warnings

---

## Completed features
- [x] Axis controls: auto / min / max for all chart series
- [x] Temperature outlier filter configuration
- [x] Atmosphere coefficient retrieval management
