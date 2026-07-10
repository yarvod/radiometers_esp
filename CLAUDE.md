# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Two independent subsystems:
1. **ESP32-S3 firmware** (root / `main/`) — C++ ESP-IDF project for a microradiometer device
2. **Cloud infrastructure** (`infra/`) — Python/FastAPI backend + Nuxt 3 frontend + supporting services

---

## Shell conventions

- Prefix **all** shell commands with `rtk` to reduce token usage (e.g. `rtk git status`, `rtk find ...`). The hook rewrites them transparently.
- **Large-output commands MUST use `rtk`** so output is filtered/compressed before reaching context. This is especially important for: `rtk idf.py build`, `rtk grep -r`, `rtk find`, `rtk docker compose`.
- `get_idf` is a shell alias that sources the ESP-IDF environment: `. $HOME/esp/esp-idf/export.sh`. Run it before any `idf.py` command in a fresh shell.
- Use `uv` for Python package management (e.g. `rtk uv pip install -r requirements.txt`).

---

## Firmware (ESP-IDF, ESP32-S3)

### Build & flash

```bash
# Source ESP-IDF first (if not already active)
get_idf

rtk idf.py set-target esp32s3
rtk idf.py build
rtk idf.py flash monitor
```

Or inside the Dev Container (`.devcontainer/Dockerfile` uses the official `espressif/idf` image):

```bash
# VS Code: Reopen in Container → then run the idf.py commands above
```

### Key firmware files

| File | Purpose |
|---|---|
| `main/app_main.cpp` | Entry point; task creation, SPI/I2C/UART init, Wi-Fi/Ethernet setup |
| `main/app_state.h/.cpp` | Global `SharedState`, `AppConfig`, `PidConfig` — single mutex-protected state object |
| `main/hw_pins.h` | All GPIO constants; change here when adapting to new board revisions |
| `main/http_handlers.cpp` | HTTP API handlers (`/data`, `/calibrate`, `/stepper/*`, etc.) |
| `main/web_ui.cpp` | Embedded web UI (served from flash/SD) |
| `main/mqtt_bridge.cpp` | MQTT publish of `{deviceId}/measure`, `{deviceId}/state`, `{deviceId}/error` |
| `main/ltc2440.cpp` | LTC2440 24-bit ADC SPI driver |
| `main/gps_unicore.cpp` | Unicore GPS UART driver (UART2, GPIO 9/10) |
| `main/wn90lp.h/.cpp` | WN90LP weather station driver (UART1, RS485, GPIO 11/12/13) |

### Architecture notes

- **SPI2 (HSPI) is shared** between three LTC2440 ADCs and the W5500 Ethernet chip; each device has its own CS pin (see `hw_pins.h`).
- State is global (`extern AppConfig app_config; extern SharedState state;`) protected by `state_mutex`. Use `CopyState()` / `UpdateState()` helpers — never lock manually.
- Configuration is loaded at boot from `config.txt` on the SD card (or NVS fallback), then merged with compiled-in defaults in `main/app_state.h`.
- USB operates in CDC (default) or MSC mode, toggled at runtime via the web UI and persisted in NVS.

### WN90LP weather station (RS485 / Modbus RTU)

- **Protocol**: Modbus RTU, default device address `0x90`, 9600 8N1, CRC16 (poly 0xA001 reflected).
- **Single bulk request** reads all 9 registers `0x0165–0x016D` in one frame: light, UVI, temperature, humidity, wind speed, gust speed, wind direction, rainfall, ABS pressure. Sensor reporting interval is 8.8 s; we **poll every `meteo_poll_interval_s` (default 9 s)** to keep `state.meteo` fresh, and **write a CSV row every `meteo_log_interval_s` (default 60 s)** — the two cadences are decoupled in the single `wn90lp` task.
- **Graceful absence**: if the station doesn't respond, `MeteoData::online = false`; the task keeps running silently. No `ESP_LOGE` on simple timeout.
- **CSV log**: each successful poll appends a row to `/sdcard/meteo_YYYYMMDD.txt` (header written once on first write).
- **Wiring** (`hw_pins.h`): `METEO_RS485_TX=GPIO13`, `METEO_RS485_RX=GPIO11`, `METEO_RS485_RTS=GPIO12`.
- Register decode rules: light × 10 lux; UVI / 10; temperature = (raw − 400) / 10 °C; wind/gust × 0.1 m/s; pressure × 0.1 hPa. Invalid sentinel: `0xFFFF` (temperature: `0x07FF`) → field set to `NaN`.

---

## Infrastructure (`infra/`)

### Starting the full stack

```bash
cd infra
rtk cp example.env .env       # fill in required vars
rtk docker compose up          # starts postgres, redis, mosquitto, minio, backend, worker, arq-worker, frontend
```

Backend auto-runs `alembic upgrade head` on startup.

### Backend (FastAPI + SQLAlchemy async)

```bash
cd infra/backend

# Install deps
rtk uv pip install -r requirements.txt -r requirements-dev.txt

# Run API server locally (needs .env or APP_* env vars)
rtk uvicorn app.main:app --reload

# Run MQTT worker (subscribes to broker, persists measurements)
rtk python -m app.worker

# Run ARQ background worker (soundings import, station refresh, scheduled jobs)
rtk arq app.arq_worker.WorkerSettings

# Run tests
rtk pytest
rtk pytest tests/test_worker_handlers.py   # single file
```

Environment variables use the `APP_` prefix (pydantic-settings); see `app/core/config.py` for all keys. For local dev, create `infra/backend/.env`.

#### Backend architecture

- **DI**: `dishka` container defined in `app/container.py`. Services and repositories are injected per-request scope; `Settings`, `AsyncEngine`, and `ArqRedis` are app-scoped.
- **Repository pattern**: interfaces in `app/repositories/interfaces.py`, SQLAlchemy implementations in `app/repositories/sqlalchemy.py`. Always depend on interfaces in services.
- **MQTT worker** (`app/worker.py`): subscribes to `+/measure`, `+/state`, `+/error`; `device_id` is the first path segment of the topic.
- **ARQ worker** (`app/arq_worker.py`): cron jobs for station data refresh (Wyoming sounding API) and sounding schedule ticks; job functions for on-demand sounding load/export.
- **Migrations**: `alembic/versions/` numbered sequentially (`00001_init.py` …). Always generate new migrations with `rtk alembic revision --autogenerate -m "description"`.

### Frontend (Nuxt 3)

```bash
cd infra/frontend
rtk npm install
rtk npm run dev      # http://localhost:3000
rtk npm run build
```

The frontend subscribes to the MQTT broker directly over WebSocket (port 9001) for live data, and calls the REST API (`NUXT_PUBLIC_API_BASE`) for historical data. State is managed in `stores/devices.ts` (Pinia). Charts use Chart.js; maps use Leaflet.

---

## Project docs (`docs/`)

| File | Purpose |
|---|---|
| `docs/WORKPLAN.md` | In-progress and TODO tasks with checkboxes |
| `docs/CHANGELOG.md` | What changed and when |
| `docs/BUGS.md` | Known open/closed bugs |

Keep these up to date as features are implemented.

---

### Deployment

CI (`.github/workflows/deploy.yml`) builds and pushes Docker images to GHCR on push to `main` when `infra/frontend/**` or `infra/backend/**` changes, then deploys with `infra/compose-prod.yml`.
