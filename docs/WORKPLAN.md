# Workplan

---

## Firmware refactoring — DONE ✅

Phases 0–10 complete. `app_main.cpp` went from 3740 → 437 lines.
All domain modules extracted to proper ESP-IDF components in `components/`.

Final binary: `0x16dc90` (2% free).

---

## WN90LP meteo station integration

The WN90LP is already wired in firmware (Phase 9): `Wn90lpClient` polls the sensor
every 60 s, writes `MeteoData` into `SharedState.meteo`. The MQTT state message and
the entire backend/frontend stack don't know about it yet.

### Step 1 — Firmware: publish meteo in state MQTT message

File: `main/mqtt_bridge.cpp`, function `BuildMqttState()`

- [ ] Append meteo fields to the state JSON (just before closing `"}"`) using `state.meteo`:
  ```json
  "meteoOnline": true,
  "meteoTemp": 12.3,
  "meteoHumidity": 65.4,
  "meteoWindSpeed": 2.1,
  "meteoGustSpeed": 3.5,
  "meteoWindDir": 270,
  "metroPressure": 1013.2,
  "meteoRainfall": 0.0,
  "meteoLight": 12000,
  "meteoUvi": 1.2,
  "meteoTimestampMs": 1700000000000
  ```
- [ ] Build green, zero warnings, binary ≤ +1 KB

---

### Step 2 — Backend: store `has_meteo` flag on Device

- [ ] Migration `00019_device_has_meteo.py`: add `has_meteo BOOLEAN NOT NULL DEFAULT FALSE` to `devices` table
- [ ] `DeviceModel` (`app/db/models.py`): add `has_meteo: Mapped[bool] = mapped_column(Boolean, default=False)`
- [ ] `Device` entity (`app/domain/entities.py`): add `has_meteo: bool = False`
- [ ] `DeviceRepository` (`app/repositories/interfaces.py`): add `upsert_meteo_config(device_id, has_meteo)` to interface
- [ ] `SqlAlchemyDeviceRepository` (`app/repositories/sqlalchemy.py`): implement `upsert_meteo_config`
- [ ] `DeviceService` (`app/services/devices.py`): expose `upsert_meteo_config`
- [ ] `handle_state` in `app/worker.py`: parse `meteoOnline` key; call `devices.upsert_meteo_config(device_id, has_meteo=True)` when it's present and truthy
- [ ] `DeviceOut` schema (`app/api/schemas.py`): add `has_meteo: bool = False`
- [ ] `GET /devices/{id}` response: confirm `has_meteo` is returned

---

### Step 3 — Frontend: live meteo card on device page

File: `infra/frontend/pages/[deviceId].vue` and `stores/devices.ts`

- [ ] `stores/devices.ts`: extend the MQTT `state` subscription to extract and store meteo fields (`meteoOnline`, `meteoTemp`, `meteoHumidity`, etc.) in the reactive device state
- [ ] `stores/devices.ts`: on initial load from `GET /devices/{id}`, store `hasMeteo` flag
- [ ] `[deviceId].vue`: add a "Метеостанция" card section, shown only when `hasMeteo == true`
  - fields: Температура, Влажность, Ветер (скорость + направление), Порывы, Давление, Осадки, Освещённость, УФ-индекс
  - values update live from MQTT state (same refresh loop as other live fields)
  - show timestamp of last successful poll (`meteoTimestampMs`)
  - show grey "offline" badge when `meteoOnline == false`

---

## Completed features
- [x] Axis controls: auto / min / max for all chart series
- [x] Temperature outlier filter configuration
- [x] Atmosphere coefficient retrieval management
