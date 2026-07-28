# AGENTS.md

This file is the maintained repository guide for coding agents. Keep it aligned
with the tree whenever an entry point, subsystem, build command, or deployment
path changes. Do not copy stale structure from old prompts or generated output.

## Repository scope

The repository contains two deployable systems:

1. ESP32-S3 firmware built with ESP-IDF.
2. Cloud infrastructure under `infra/`: FastAPI workers and API, Nuxt frontend,
   and the supporting Docker Compose services.

## Repository map

| Path | Role |
|---|---|
| `CMakeLists.txt`, `sdkconfig*`, `partitions.csv` | ESP-IDF project and board/build configuration |
| `main/` | Firmware composition layer: application entry point, HTTP/Web UI, control actions, and MQTT bridge |
| `components/app_core/` | Shared firmware state, context, utilities, error manager, and GPIO definitions |
| `components/config_loader/` | SD/NVS configuration loading and persistence |
| `components/data_logger/` | Measurement logging |
| `components/gps_module/` | GNSS receiver and positioning logic |
| `components/motion_controller/` | Motor, relay, and motion control |
| `components/network_manager/` | Wi-Fi, Ethernet, and network failover |
| `components/sensor_hub/` | LTC2440 and temperature sensor acquisition |
| `components/storage_manager/` | SD/storage ownership |
| `components/upload_pipeline/` | File maintenance and remote upload |
| `components/wn90lp/` | WN90LP Modbus weather-station driver |
| `managed_components/` | ESP-IDF managed dependencies; do not hand-edit during normal feature work |
| `tests/firmware/` | Host-side firmware test harness and stubs |
| `infra/backend/app/api/routes/` | FastAPI HTTP route adapters |
| `infra/backend/app/services/` | Backend application/domain services |
| `infra/backend/app/repositories/` | Repository interfaces and SQLAlchemy implementations |
| `infra/backend/app/db/` | Database models and sessions |
| `infra/backend/app/worker.py` | MQTT ingestion worker |
| `infra/backend/app/arq_worker.py` | ARQ jobs and scheduled work |
| `infra/backend/alembic/` | Database migrations |
| `infra/backend/tests/` | Backend pytest suite |
| `infra/frontend/pages/` | Nuxt routes; `[deviceId].vue` is the device-page shell |
| `infra/frontend/components/device/` | Device feature tabs and their owned components |
| `infra/frontend/stores/devices.ts` | Pinia device state and MQTT actions |
| `infra/frontend/plugins/mqtt.client.ts` | Browser MQTT connection |
| `infra/frontend/utils/`, `types/` | Shared frontend utilities and contracts |
| `infra/docker-compose.yml` | Local development stack |
| `infra/compose-prod.yml` | Production service definitions |
| `infra/scripts/` | Backup and restore scripts |
| `.github/workflows/deploy.yml` | Docker image build, push, and production deployment |
| `docs/` | Workplan, changelog, and known bugs |

Generated or machine-local directories are not source: `build/`,
`infra/frontend/node_modules/`, `infra/frontend/.nuxt/`,
`infra/frontend/.output/`, `infra/.venv/`, `.pytest_cache/`, and `.DS_Store`.
Do not document generated files as architecture and do not commit them.

## Shell conventions

- Prefix every shell command and every segment of a command chain with `rtk`.
- Use `rg`/`rg --files` for search, invoked through `rtk proxy` when needed.
- Run commands from the subsystem directory shown below so configuration and
  relative paths resolve consistently.
- Preserve unrelated working-tree changes.

## Architecture boundaries

### Firmware

- `main/app_main.cpp` composes components; reusable hardware and domain logic
  belongs in the matching directory under `components/`.
- Shared state and pins live in `components/app_core/`, not `main/`.
- Access shared state through the helpers/context provided by `app_core`; keep
  synchronization ownership there.
- Register new firmware components in their own `CMakeLists.txt` and in
  `main/CMakeLists.txt` requirements.
- `managed_components/` is controlled by the ESP-IDF component manager.

### Backend

- Routes translate HTTP schemas and delegate to services.
- Services depend on repository interfaces, not SQLAlchemy implementations.
- Dependency wiring is in `app/container.py`.
- MQTT topic ingestion is owned by `app/worker.py`; scheduled/background jobs
  are owned by `app/arq_worker.py`.
- Add schema changes as a new sequential migration under
  `infra/backend/alembic/versions/`; do not rewrite applied migrations.
- Runtime settings use the `APP_` prefix and are defined in
  `app/core/config.py`.

### Frontend

- Nuxt routes stay thin. Device-tab behavior belongs under
  `components/device/`, with feature-specific children under `data/`, `gps/`,
  and `meteo/`.
- REST access goes through `composables/useApi.ts`; live device state and MQTT
  commands go through `stores/devices.ts`.
- Browser-only integrations belong in `.client.ts` plugins or guarded client
  code.
- Shared date/time, CSV, and chart contracts belong in `utils/` and `types/`,
  not duplicated across components.
- Keep `package.json` and `package-lock.json` synchronized. Validation uses
  `npm ci`; use `npm install` only when intentionally changing dependencies and
  commit the resulting lock-file update.
- The deployment toolchain versions are pinned in `infra/frontend/Dockerfile`
  and `package.json`. Update them deliberately and verify a clean image build.

## Required verification

Run the checks for every subsystem touched. A partial check must be reported as
partial; do not say “the build passes” when only one layer was tested.

### Frontend changes

From `infra/frontend/`:

```bash
rtk npm run build
```

From the repository root, always verify the clean deployment path as well:

```bash
rtk docker build --no-cache --progress=plain --platform linux/amd64 --target runner \
  -t radiometer-frontend-check infra/frontend
```

The `linux/amd64` Docker build is mandatory for frontend source, dependency,
lock-file, or Dockerfile changes because it matches the GitHub-hosted deployment
build, independently runs `npm ci` in Linux, and then runs the Nuxt build. A
local `npm run build` with an existing `node_modules` directory is not a
substitute. If Docker is unavailable, state explicitly that deployment build
verification remains outstanding.

When dependencies change, additionally run a clean install before the local
build:

```bash
rtk npm ci
rtk npm run build
```

### Backend changes

From `infra/backend/`:

```bash
rtk pytest
rtk python -m compileall -q app tests
```

For backend code, requirements, migrations, or Dockerfile changes, also run from
the repository root:

```bash
rtk docker build --no-cache --progress=plain --platform linux/amd64 \
  -t radiometer-backend-check infra/backend
```

### Firmware changes

Load the installed ESP-IDF environment (`get_idf` in the configured developer
shell), then run from the repository root:

```bash
rtk idf.py build
```

Run the relevant host tests under `tests/firmware/` when their source/stub map
covers the component being changed. Do not treat a host test as a replacement
for the ESP-IDF build.

### Documentation-only changes

At minimum:

```bash
rtk git diff --check
```

### Final check

Before handoff:

```bash
rtk git diff --check
rtk git status --short
```

Report the exact checks that ran, their outcomes, and any check that could not
run. Warnings should be distinguished from failures.

## Docker and deployment

- Frontend production images use the final `runner` stage in
  `infra/frontend/Dockerfile`.
- `.dockerignore` must exclude host dependencies and Nuxt build output so macOS
  or stale local artifacts cannot overwrite Linux dependencies in the image.
- Local services use `infra/docker-compose.yml`; production uses
  `infra/compose-prod.yml`. Do not mix their assumptions.
- `.github/workflows/deploy.yml` builds changed frontend/backend images, pushes
  them to GHCR, and deploys the production Compose file on `main`.
- Never commit `.env`, credentials, certificates containing private keys, or
  generated production data.

## Documentation maintenance

- Update this file in the same change whenever paths, ownership boundaries,
  required checks, Docker stages, or deployment flow change.
- Update `docs/WORKPLAN.md` when active rollout work changes.
- Update `docs/BUGS.md` when a known defect is opened or closed.
- Update `docs/CHANGELOG.md` for user-visible or operational behavior intended
  for release.
- Prefer links to canonical files over duplicating volatile implementation
  details here.
