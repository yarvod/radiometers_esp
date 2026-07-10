# Changelog

## 2026-07-10 — Meteo measurement linking corrective review

- Added normalized `meteo_readings` storage and nullable measurement FK with atomic
  deduplication by `(device_id, timestamp_ms)`.
- Added WN90LP snapshots to `/measure` payloads and separated station polling from CSV
  logging.
- Hardened MQTT telemetry parsing against invalid UTF-8, non-object JSON, non-finite
  values and invalid timestamps; isolated per-message failures from the worker loop.
- Added legacy config migration from the old 60-second meteo poll default to 9 seconds.
- Replaced wall-clock cadence arithmetic with independent monotonic poll/log deadlines.
- Made meteo CSV writes report success/failure and retry after transient storage errors.
- Made meteo upsert refresh fields on duplicate delivery and added allocation checks for
  the firmware cJSON meteo subtree.
- Verification: 39 backend tests pass; Python compileall passes; ESP-IDF build passes
  with binary size `0x166690` and 30% application partition headroom.
