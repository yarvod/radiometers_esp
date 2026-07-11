# Changelog

## 2026-07-11 — Device page decomposition and meteo history

- Reduced `pages/[deviceId].vue` from 3769 lines to a route shell and moved Data,
  Control, GPS/GNSS, Meteo, Settings, and Errors into feature-owned components.
- Combined local UM982 state/config and processed GNSS dataset administration in the
  `GPS` tab; legacy `?tab=gnss` links normalize to `?tab=gps`.
- Added bounded `/api/meteo-readings` history with indexed direct reads,
  server-side auto/manual aggregation, circular wind-direction averaging, maximum
  gust, and the last non-null rainfall value per bucket.
- Added independent meteo date/limit/bucket/auto-refresh controls and six lazy Chart.js
  groups below the live station state, including stale-request, timer, and chart cleanup.
- Preserved tab forms with `KeepAlive`; live MQTT updates no longer overwrite dirty
  PID, heater, motor, Wi-Fi, or network inputs.
- Restored the measurement outlier-filter response contract and made meteo buckets
  range-anchored so automatic and manual aggregation cover the newest sample without
  silently truncating the selected interval.
- Added timezone-aware range validation, mandatory date bounds and regression tests
  for response schemas and bucket boundary coverage.
- Replaced PostgreSQL-incompatible `mod(double precision, double precision)` in the
  circular wind-direction aggregate with a `floor`-based normalization verified on
  the production PostgreSQL 15 dataset.
- Restored firmware-compatible PID mask/index decoding, confirmations for dangerous
  commands, unknown-value rendering, MinIO uptime semantics and legacy Wi-Fi mode.
- Kept calibration state alive across tabs while cancelling its sampling loop on
  deactivation; cached Errors and Settings now refresh without overwriting dirty forms.
- Extracted shared history types and Chart.js ownership into measurement/atmosphere
  panels with legend-state and lifecycle preservation.
- Verification: 48 backend tests pass, Python compileall passes, Alembic has one
  `00021` head, and the Nuxt production build passes.

## 2026-07-10 — Meteo measurement linking corrective review

- Added normalized `meteo_readings` storage and nullable measurement FK with atomic
  deduplication by `(device_id, timestamp_ms)`.
- Added WN90LP snapshots to `/measure` payloads and separated station polling from CSV
  logging.
- Hardened MQTT telemetry parsing against invalid UTF-8, non-object JSON, non-finite
  values and invalid timestamps; isolated per-message failures from the worker loop.
- Added explicit independent config keys: `meteo_poll_interval_s=9` for station/state
  refresh and `meteo_file_interval_s=60` for CSV writes; no compatibility aliases or
  hidden period conversion are used.
- Replaced wall-clock cadence arithmetic with independent monotonic poll/file deadlines.
- Made meteo CSV writes report success/failure and retry after transient storage errors.
- Prevented the startup upload sweep from moving the active meteo CSV and made failed
  hourly rotation retry instead of silently abandoning the completed file.
- Made meteo upsert refresh fields on duplicate delivery and added allocation checks for
  the firmware cJSON meteo subtree.
- Verification: 39 backend tests pass; Python compileall passes; ESP-IDF build passes
  with binary size `0x166650` and 30% application partition headroom.
