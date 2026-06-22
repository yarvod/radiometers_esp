# Workplan

---

## Firmware refactoring — DI / module split

**Goal:** `app_main.cpp` (4230 lines) → entry point ~300 lines. Each domain becomes an isolated `.h`/`.cpp` module. `AppContext` is the DI container passed explicitly to every FreeRTOS task. No global `extern` access to config/state outside of designated accessors.

### Acceptance criteria (every phase)
1. `rtk idf.py build` exits 0 — zero errors, zero new warnings
2. Binary size ≤ previous phase + 2 KB (or justified in commit message)
3. All `extern` references to moved symbols are gone from `app_main.cpp`
4. No new file-scope `static` state added to `app_main.cpp`
5. Commit message records: lines removed from `app_main.cpp`, binary size delta

### Phase 0 — Prerequisites ✅ DONE
- [x] Remove `inline constexpr char TAG[] = "APP"` from `app_state.h` (was leaking into every TU)
- [x] `app_main.cpp`: add local `static constexpr char kTag[] = "APP"`, replace all 153 `TAG` → `kTag`
- [x] `http_handlers.cpp`: local `static constexpr char kTag[] = "HTTP"`, replace 48 `TAG` → `kTag`
- [x] `wn90lp.cpp`: uses `kTag` from day one — no collision

---

### Phase 1 — `AppContext` skeleton + `ConfigLoader` 🔜 NEXT
**Risk: LOW** — pure parsing logic, no task changes, no mutex touching.

Files to create: `main/app_context.h`, `main/config_loader.h`, `main/config_loader.cpp`

**`AppContext`** — skeleton only, grows in later phases:
```cpp
struct AppContext {
  AppConfig&        config;
  PidConfig&        pid_config;
  // modules added here in subsequent phases
};
```

**`ConfigLoader`** extracts from `app_main.cpp`:
- `ParseConfigText()` (~371 lines of key=value parsing)
- `ParseConfigFile()`, `LoadConfigFromSdCard()`, `LoadConfigFromInternalFlash()`
- `SaveConfigToSdCard()`, `SaveConfigToInternalFlash()`, `SyncConfigToInternalFlash()`

`app_services.h` keeps the same declarations → `ConfigLoader` implements them → zero changes to callers.

**Test / acceptance:** build green, `app_main.cpp` loses ~550 lines, size delta ≤ +1 KB (strings move, no new code).

**Commit:** `refactor(fw): extract ConfigLoader, add AppContext skeleton`

---

### Phase 2 — `StorageManager`
**Risk: MEDIUM** — `SdLockGuard` API changes touch every file that uses SD.

Files to create: `main/storage_manager.h`, `main/storage_manager.cpp`

`StorageManager` owns: `sd_mutex`, `sd_card`, `log_sd_card`, `log_sd_mounted`, all mount/unmount/path functions.

Key API change — `SdLockGuard` constructor:
```cpp
// Before (grabs global sd_mutex):
SdLockGuard guard;
// After (takes StorageManager ref):
SdLockGuard guard(ctx.storage);
```
Mechanical replacement at all ~25 call sites. `AppContext` grows `StorageManager storage`.

**Test / acceptance:** build green, `app_main.cpp` loses ~250 lines, `app_state.cpp` loses `SdLockGuard` impl.

**Commit:** `refactor(fw): extract StorageManager, SdLockGuard takes explicit storage ref`

---

### Phase 3 — `NetworkManager`
**Risk: MEDIUM** — large, many event handlers, file-scope state.

Files to create: `main/network_manager.h`, `main/network_manager.cpp`

Owns: wifi/eth init, event handlers, SNTP, `EnsureTimeSynced`, all `wifi_*` + `eth_*` file-scope vars, `WifiMonitorTask`, `WifiRecoverTask`, `NetworkMonitorTask`.

`AppContext` grows `NetworkManager network`. Tasks that need network status receive `AppContext*`.

**Test / acceptance:** build green, `app_main.cpp` loses ~700 lines.

**Commit:** `refactor(fw): extract NetworkManager`

---

### Phase 4 — `UploadPipeline`
**Risk: LOW-MEDIUM** — self-contained, depends on StorageManager + AppConfig.

Files to create: `main/upload_pipeline.h`, `main/upload_pipeline.cpp`

Owns: `UploadFileToMinio` (256 lines + SigV4 helpers), `UploadPendingOnce`, file-type detection (`IsDataLogFilename`, `IsGpsLogFilename`, `IsMeteoLogFilename`), `MoveRootXxxFilesToUploadLocked` helpers.

`UploadTask` shrinks to ~10 lines calling `ctx.upload.runOnce()`.

**Test / acceptance:** build green, `app_main.cpp` loses ~450 lines.

**Commit:** `refactor(fw): extract UploadPipeline`

---

### Phase 5 — `DataLogger`
**Risk: LOW** — owns clearly-bounded state.

Files to create: `main/data_logger.h`, `main/data_logger.cpp`

Owns: `log_file`, `current_log_path`, `log_config`, `OpenLogFileWithPostfix`, `FlushLogFile`, `StartLoggingToFile`, `StopLogging`, `BuildLogFilename`, `QueueCurrentLogForUpload`, log CSV row-write path.

`AppContext` grows `DataLogger data_logger`. `AdcTask` receives `AppContext*`, calls `ctx.data_logger.writeRow(...)`.

**Test / acceptance:** build green, `app_main.cpp` loses ~250 lines, `log_file` is no longer `extern`.

**Commit:** `refactor(fw): extract DataLogger`

---

### Phase 6 — `SensorHub`
**Risk: LOW** — hardware init + tasks, minimal external deps.

Files to create: `main/sensor_hub.h`, `main/sensor_hub.cpp`

Owns: LTC2440 ×3 instances, INA219 (`i2c_bus`, `ina219_dev`), 1-Wire, fan tach ISR, `InitSpiBus`, `ReadAllAdc`, `InitIna219`, `ReadIna219`, `BuildTempMeta`. Tasks (`AdcTask`, `Ina219Task`, `TempTask`, `FanTachTask`) become `SensorHub` methods.

`AppContext` grows `SensorHub* sensors`. Tasks receive `AppContext*`.

**Test / acceptance:** build green, `app_main.cpp` loses ~250 lines.

**Commit:** `refactor(fw): extract SensorHub`

---

### Phase 7 — `MotionController`
**Risk: LOW** — `control_actions.cpp` already partially separate.

Files to create: `main/motion_controller.h`, `main/motion_controller.cpp`

Owns stepper, heater/fan PWM, calibration. `control_actions.cpp` becomes thin delegator to `MotionController`. `StepperTask`, `PidTask`, `CalibrationTask`, `FindZeroTask` become methods.

`AppContext` grows `MotionController* motion`.

**Test / acceptance:** build green, `app_main.cpp` loses ~400 lines, `control_actions.cpp` shrinks to delegators.

**Commit:** `refactor(fw): extract MotionController`

---

### Phase 8 — `GpsModule`
**Risk: HIGH** — most complex, depends on storage + upload + time.

Files to create: `main/gps_module.h`, `main/gps_module.cpp`

Wraps `GpsUnicoreClient`. Owns `GpsLogTask` logic, `current_gnss_log_path`, `gps_reconfigure_requested`, `RotateStaleGnssLogLocked`, `MoveRootGpsFilesToUploadLocked`. Free functions in `app_services.h` (`GetGpsCurrentMode`, `GetGpsReceiverStatus`, `RequestGpsPositionOnce`) become `GpsModule` methods; `app_services.h` declarations become thin wrappers for backward compat or are removed.

`AppContext` grows `GpsModule* gps`.

**Test / acceptance:** build green, `app_main.cpp` loses ~350 lines, `app_services.h` GPS section removed.

**Commit:** `refactor(fw): extract GpsModule`

---

### Phase 9 — Final `app_main.cpp` cleanup
After all 8 modules extracted:
- `app_main()` is ~300 lines: GPIO init, `AppContext` construction, task spawn table
- `app_services.h` shrinks from 130 → ~20 lines (time utils, `IsoUtcNow`, `FormatUtcIso`)
- Wire `Wn90lpClient` into `AppContext` as `meteo` member
- Remove all `extern` that no longer need to be extern
- Enable `CONFIG_COMPILER_OPTIMIZATION_SIZE` for non-critical TUs if binary is still tight

**Commit:** `refactor(fw): finalize app_main as entry point, wire meteo into AppContext`

---

## WN90LP weather station integration

### Done
- [x] `main/wn90lp.h/.cpp` — Modbus RTU, bulk read, CRC16, graceful absent-station, hourly rotation, storage-agnostic
- [x] `app_config.meteo_poll_interval_s` / `meteo_enabled` — runtime config from `config.txt`
- [x] `SharedState.meteo`, S3 prefix `meteo/`, `MoveRootMeteoFilesToUploadLocked`

### TODO (after Phase 9 — wired into AppContext)
- [ ] `/data` HTTP response: add meteo fields from SharedState (no new endpoints)
- [ ] MQTT `state` publish: include meteo snapshot in `{deviceId}/state` JSON
- [ ] Backend: detect `meteoOnline` in state → `upsert_meteo_config`, `has_meteo: bool` on Device, migration
- [ ] Frontend: "Meteo station" card on device detail (visible when `has_meteo == true`)
- [ ] **Measurements — DEFERRED**: standalone `MeteoReading` table; don't mix with ADC stream

---

## Completed
- [x] Axis controls: auto / min / max for all chart series
- [x] Temperature outlier filter configuration
- [x] Atmosphere coefficient retrieval and management
