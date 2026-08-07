# Changelog

## 2026-08-08 — Larger S3 recovery objects

- Raised the S3 recovery import limit from 1 MiB to 1 GiB and exposed the
  limit through `APP_S3_MAX_OBJECT_BYTES` in local and production Compose.

## 2026-08-06 — Restore S3 recovery job startup

- Prevented the async SQLAlchemy repository from implicitly reloading the
  server-managed `updated_at` column after claiming an S3 recovery job, which
  previously raised `MissingGreenlet` before the worker could inspect MinIO.
- Added regression coverage for lease, schedule, error and timestamp updates
  performed while claiming a device sync job.

## 2026-08-05 — Per-device S3 recovery import

- Added configurable per-device recovery of radiometer and meteo CSV files from
  a shared MinIO/S3 endpoint through the ARQ worker.
- Added independent per-prefix cursors plus an object/ETag processing ledger, so
  old files uploaded after newer files are still discovered without reimporting
  unchanged objects.
- Added header-driven Pydantic CSV validation compatible with radiometer files
  that predate calibration and GPS columns; file `timestamp_ms` values are
  ignored.
- Added canonical content fingerprints at the firmware CSV precision. MQTT and
  S3 copies of the same sample deduplicate atomically, while distinct readings
  within the same `timestamp_iso` second remain intact.
- Removed timestamp-only uniqueness from radiometer and meteo readings and made
  migration `00023` restart-safe after a partially applied deployment.
- Added authenticated device endpoints for reading/updating S3 recovery
  settings and requesting an immediate run, with per-device intervals, enable
  controls, progress counters, cursors, and last-error state.
- Added a Nuxt device-settings panel for configuring the bucket, schedule,
  prefixes and batch size, viewing recovery progress/errors, and triggering an
  immediate file check without exposing shared MinIO credentials to the browser.
- Files dated 1970 are recorded as ignored, malformed files are retried without
  blocking later files, and missing optional `meteo/` content is accepted.

## 2026-07-29 — Scalable filtered measurement history

- Replaced full ORM materialization of Hampel-filtered ranges with a server-side
  cursor, 20,000-point batches, exact window overlap, and incremental bucket
  aggregation.
- Added a composite `(device_id, timestamp, id)` measurement index, created
  concurrently during migration.
- Added a unified device-history endpoint so ordinary and atmospheric charts
  reuse one filtered measurement series instead of querying it twice.
- Added an explicit calculation spinner/status to the device data tab and made
  PWV coefficient recalculation user-triggered instead of firing on every edit.
- Configured two Uvicorn worker processes by default in production Compose; the
  count remains configurable through `UVICORN_WORKERS`.

## 2026-07-29 — Reproducible frontend and CI builds

- Pinned the frontend container to Node 24.16.0 on Alpine 3.22 and recorded
  npm 11.13.0 as the project package manager, replacing the floating Node 20
  image and its npm 10 dependency installer.
- Made both development and production build stages inherit the same clean
  `npm ci` dependency layer.
- Added a frontend `.dockerignore` so host `node_modules`, `.nuxt`, and
  `.output` directories cannot enter or overwrite Linux image contents.
- Updated GitHub Actions to Node 24-compatible `checkout` and `paths-filter`
  releases and removed the unused host `setup-node` step.
- Replaced the stale agent guide with the current firmware/backend/frontend
  structure and an explicit verification matrix that requires both Nuxt and
  clean Docker builds for frontend changes.

## 2026-07-16 — Configurable Ethernet IPv4

- Added DHCP/manual Ethernet selection to the embedded local WebUI, including
  IPv4 address, netmask, and gateway fields.
- Added the same Ethernet controls to the Nuxt device page over MQTT, with
  client-side IPv4 validation and dirty-form protection from live state updates.
- Kept both interfaces active in Wi-Fi + Ethernet mode and made repeated MQTT
  disconnects rotate the default route to the other interface, with an immediate
  reconnect and cooldown to avoid route flapping.
- Added an explicit static Ethernet DNS setting to both UIs and MQTT, defaulting
  to `8.8.8.8`, since a static interface has no DHCP lease from which to learn DNS.
- Persisted Ethernet IPv4 settings in `config.txt` and its NVS backup, validated
  manual addresses before save, and applied static addressing to the W5500 before start.

## 2026-07-15 — Restore live GNSS position after receiver reconfiguration

- Restored continuous position updates on UM982 COM2 by configuring `GPGGA`
  once per second alongside periodic ZDA and RTCM output.
- Made ZDA/GGA dispatch independent of the GP/GN talker prefix, matching UM982
  responses such as `$GNZDA` to commands issued as `GPZDA`.

## 2026-07-11 — Runtime meteo interval configuration

- Added one validated firmware action for WN90LP polling (`1..3600` seconds) and
  CSV writes (`10..86400` seconds), shared by MQTT and the local HTTP UI.
- Meteo interval changes take effect without restart and are accepted only when
  the generated `config.txt` is synchronized successfully to both SD and its NVS
  backup; partial saves are rolled back to the previous values.
- Published the configured intervals in device state and added editors to the
  Nuxt Meteo tab and the local embedded web UI.
- Made the Data tab initialize its history range immediately to the last 24 hours
  in the browser timezone, using the same local-input/UTC-query conversion as Meteo.
- Hardened that default range across initial mount and KeepAlive activation so the
  date inputs are populated before asynchronous history/config loading begins.
- Corrected MinIO telemetry so attempts count real enabled queued-file processing
  rather than empty/disabled polling cycles; added remote success/failure and local
  archive-failure counters plus an uptime-based last-result age to both UIs.
- Made MinIO diagnostic counter updates wait for the state mutex instead of being
  silently dropped, and hardened file streaming against short HTTP writes/read errors.

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
