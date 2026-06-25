# Workplan

## GNSS PWV datasets for device data tab

Status: implemented in `infra/backend` and `infra/frontend`; GNSS source CRUD/import lives in a dedicated device tab, while the data tab keeps the PWV chart overlay. Remaining operational validation is running the new migration against a real Postgres instance and smoke-testing with production-size GNSS files.

Goal: allow operators to upload already processed GNSS observation files, store them as named datasets attached to a device, and overlay their PWV time series on the existing PWV chart next to radiometer/profile-derived PWV for the selected time window.

Input format:

```text
% yy mn dd hh mm ss PW(mm) sPW(mm) T(C)
2026 05 17 10 00 00 6.47 5.22 5.25
2026 05 17 11 00 00 6.47 4.66 5.25
```

The model should support more than one GNSS source per device:

- built-in GNSS processed together with the radiometer device;
- nearby independent GNSS station at the same point;
- future manually named comparison sources.

### Step 1 - Data model and migration

- [ ] Add migration `00020_gnss_data.py`.
- [ ] Add table `gnss_data`:
  - `id String(36) primary key`, UUID default;
  - `device_id String(64) not null references devices(id) on delete cascade`;
  - `name String(128) not null`;
  - `description Text nullable`;
  - `measurement_count Integer not null default 0`;
  - `start_at DateTime(timezone=True) nullable`;
  - `end_at DateTime(timezone=True) nullable`;
  - `last_import_at DateTime(timezone=True) nullable`;
  - `created_at DateTime(timezone=True) server_default now()`;
  - `updated_at DateTime(timezone=True) server_default now() on update`;
  - unique constraint `(device_id, name)` so one device cannot have two same-named GNSS rows.
- [ ] Add table `gnss_data_measurements`:
  - `id String(36) primary key`, UUID default;
  - `gnss_data_id String(36) not null references gnss_data(id) on delete cascade`;
  - `measured_at DateTime(timezone=True) not null`;
  - `pw_mm Float not null`;
  - `spw_mm Float nullable`;
  - `temperature_c Float nullable`;
  - `created_at DateTime(timezone=True) server_default now()`;
  - `updated_at DateTime(timezone=True) server_default now() on update`;
  - unique constraint `(gnss_data_id, measured_at)`.
- [ ] Add indexes:
  - `ix_gnss_data_device_id` on `gnss_data(device_id)`;
  - `ix_gnss_data_measurements_dataset_time` on `gnss_data_measurements(gnss_data_id, measured_at)`.
- [ ] Add SQLAlchemy models in `infra/backend/app/db/models.py` and relationships to `DeviceModel`.
- [ ] Add domain dataclasses in `infra/backend/app/domain/entities.py`.

Performance notes:

- The `(gnss_data_id, measured_at)` unique constraint is the import idempotency boundary.
- The `(gnss_data_id, measured_at)` index is also the chart query path; every chart request must include `from` and `to` when the UI has a selected range.
- Store dataset bounds and counts on `gnss_data` so the UI can list datasets without running `count(*)` or `min/max` over measurements on every page load.

### Step 2 - Parser and import service

- [ ] Add `GnssDataRepository` interface and SQLAlchemy implementation.
- [ ] Add `GnssDataService` with methods:
  - list datasets for a device;
  - create/update/delete dataset metadata;
  - import processed GNSS text into a dataset;
  - list measurement points for selected datasets and time range.
- [ ] Parser rules:
  - accept UTF-8 text upload;
  - skip blank lines;
  - skip comment/header lines starting with `%`;
  - parse whitespace-separated rows with 9 columns: `yyyy mm dd hh mm ss pw_mm spw_mm temperature_c`;
  - normalize timestamps to timezone-aware UTC;
  - reject rows with invalid dates or non-finite PW values;
  - keep line-level errors for response summary, but do not fail the whole upload unless there are no valid rows.
- [ ] Deduplicate rows inside one uploaded file by timestamp before DB write; last valid row wins and increments a duplicate counter.
- [ ] Bulk upsert with PostgreSQL `insert(...).on_conflict_do_update(...)` on `(gnss_data_id, measured_at)`:
  - update `pw_mm`, `spw_mm`, `temperature_c`, `updated_at`;
  - insert missing rows;
  - execute in chunks, default `5000` rows per statement.
- [ ] Keep parse/validation outside the DB transaction where possible; open the transaction only for dataset lookup/create and chunked upsert.
- [ ] After import, update `measurement_count`, `start_at`, `end_at`, `last_import_at` for that dataset.
- [ ] Return import summary: parsed rows, inserted/updated approximation or upserted count, duplicate rows, skipped rows, first/last timestamp, and first few validation errors.

Performance notes:

- Do not delete and recreate a dataset during import; idempotent upsert avoids table churn and chart gaps.
- Do not ORM-add measurements one by one; use Core bulk insert/upsert.
- Avoid one giant transaction for huge files if later uploads can be large: parse all rows, then upsert chunks in a single short transaction for normal files; if files become very large, allow chunk commits with final bounds refresh.

### Step 3 - Backend API

- [ ] Add route module `infra/backend/app/api/routes/gnss_data.py` and register it in `routes/__init__.py` and `main.py`.
- [ ] Add schemas in `infra/backend/app/api/schemas.py`:
  - `GnssDataOut`;
  - `GnssDataCreateRequest`;
  - `GnssDataUpdateRequest`;
  - `GnssDataMeasurementPointOut`;
  - `GnssDataSeriesOut`;
  - `GnssDataImportResponse`.
- [ ] Endpoints:
  - `GET /api/devices/{device_id}/gnss-data` - list named datasets with bounds/counts;
  - `POST /api/devices/{device_id}/gnss-data` - create dataset metadata;
  - `PATCH /api/devices/{device_id}/gnss-data/{gnss_data_id}` - rename/change description;
  - `DELETE /api/devices/{device_id}/gnss-data/{gnss_data_id}` - delete dataset and cascade measurements;
  - `POST /api/devices/{device_id}/gnss-data/{gnss_data_id}/import` - multipart text file upload;
  - `GET /api/devices/{device_id}/gnss-data/series?from=...&to=...&ids=...` - return points grouped by dataset for chart overlay.
- [ ] Enforce auth via existing `get_current_user` dependency.
- [ ] Validate dataset ownership by both `device_id` and `gnss_data_id`; never load a dataset only by id.
- [ ] Add response limit protection:
  - require or infer a time window for chart series;
  - cap returned points per dataset, e.g. `limit_per_dataset=10000`;
  - if a dataset exceeds the cap, either return sampled/aggregated points or a flag telling the UI to narrow the range.

### Step 4 - Frontend data-tab UX

- [ ] Extend `infra/frontend/pages/[deviceId].vue` with GNSS dataset state/types:
  - dataset list;
  - selected dataset ids for overlay;
  - import form state;
  - import result/status.
- [ ] In the existing `Данные` tab, add a compact GNSS management block near the PWV controls:
  - list named GNSS sources for this device;
  - checkboxes/toggles to show/hide each source on the PWV chart;
  - create dataset form: name and optional description;
  - upload control for the selected dataset;
  - import summary after upload.
- [ ] Load dataset metadata when the device page opens and after create/import/delete.
- [ ] Load GNSS series together with `loadHistory()` / `loadAtmosphere()` using the same `historyFilters.from`, `historyFilters.to`, and selected dataset ids.
- [ ] Add GNSS points into `buildPwvAtmosphereDatasets()`:
  - extend the existing PWV timeline with GNSS timestamps;
  - create one Chart.js dataset per `gnss_data`;
  - label series with dataset `name`;
  - use stored `color` or deterministic palette fallback;
  - style GNSS lines differently from profile/radiometer series, e.g. solid line with point radius 2.
- [ ] Preserve current chart legend hidden-state behavior when datasets are refreshed.
- [ ] Show empty/limit states:
  - no GNSS datasets yet;
  - selected dataset has no points in current period;
  - server capped result and asks to narrow time range.

### Step 5 - Tests

- [ ] Backend parser unit tests:
  - header/comment skip;
  - blank line skip;
  - valid sample file;
  - invalid row reporting;
  - duplicate timestamp in file uses last value.
- [ ] Repository/service tests with test database:
  - create/list/update/delete dataset;
  - import inserts new points;
  - repeated import with same timestamps updates values instead of duplicating;
  - multiple GNSS datasets on one device stay separate;
  - series endpoint filters by device, dataset ids, and time window.
- [ ] API tests:
  - auth required;
  - dataset ownership checks;
  - multipart import response summary;
  - chart series cap behavior.
- [ ] Frontend verification:
  - build succeeds with new types;
  - manual smoke in browser: create two GNSS datasets, import sample file, select both, verify PWV chart legend and overlay.

### Step 6 - Rollout and operational checks

- [ ] Run migration locally and verify indexes exist.
- [ ] Import realistic file sizes and record timings for:
  - first import;
  - repeated import with all timestamps already present;
  - chart query for 24 h, 7 d, and 30 d windows.
- [ ] Verify database query plans use `ix_gnss_data_measurements_dataset_time`.
- [ ] Add a short changelog entry in `docs/CHANGELOG.md` after implementation.

## Open decisions before implementation

- [ ] Confirm whether uploaded timestamps should be interpreted as UTC. Current plan assumes UTC because the file has no timezone.
